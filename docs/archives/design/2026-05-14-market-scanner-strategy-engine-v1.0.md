# Market Scanner, Strategy Framework & Signal Engine

**Version:** 1.0
**Date:** 2026-05-14
**Status:** ✅ DONE - Implemented

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-05-14 | Initial design: MarketScanner (WebSocket kline cache), IStrategy interface, SignalEngine sequential work-queue, ATR-based TP/SL, time-exit, per-strategy sizing |

---

## 1. Muc Tieu

Thiet ke ba module moi tren nen tang C++23 Binance Futures SDK hien co:

1. **MarketScanner** — subscribe WebSocket kline streams cho toan bo futures symbols, duy tri in-memory OHLCV cache, phuc vu du lieu cho strategy evaluation ma khong ton rate limit REST.

2. **IStrategy / StrategyRegistry** — interface plugin chung cho strategy. Cho phep phat trien nhieu strategy doc lap, moi strategy tu khai bao config (intervals, scan_interval, max_hold_duration, sizing params). Strategy cu the se thiet ke o giai doan sau; giai doan nay chi thiet ke khung.

3. **SignalEngine** — main loop xu ly sequential work-queue gom cac item `(symbol, interval, strategy)`. Khi co signal → tinh size theo ATR-risk formula → dat lenh market → dat TP/SL theo ATR multiplier. Mo phong lenh o ca hai chieu long/short. Fallback time-exit neu khong hit TP/SL sau `max_hold_duration`. Sau khi het queue → wait `scan_interval` → repeat.

---

## 2. Understanding Lock

### 2.1 Summary

- **Market Scanner**: Subscribe WebSocket kline streams cho ~2000 futures symbols. Timeframes configurable, bat dau voi `15m` va `30m`. In-memory cache moi symbol × interval → `vector<Kline>` kich co fixed buffer (200 candles).
- **Strategy Framework**: Plugin interface `IStrategy`. Moi strategy nhan `(symbol, interval, klines)` → tra `Signal` co direction (Long/Short/None), confidence, va ATR. Config per-strategy: `scan_interval`, `max_hold_duration`, `risk_pct`, `sl_multiplier`, `takeProfitPercent`, `tp_multiplier`, `min_notional`, `atr_period`.
- **Signal Engine**: Sequential work-queue `vector<WorkItem{symbol, interval, strategy*}>`. Round-robin la cach phan cong (symbol × interval) sang strategy, xu ly tuan tu. Gap signal → execute. Skip symbol neu da co open position (one-way mode). Sau khi het queue → sleep theo `scan_interval` cua strategy → rescan.
- **Position Sizing**: `size = max(min_notional, balance × risk_pct / (atr × sl_multiplier))`. Per-strategy config. Min $1 cho account < $50.
- **TP/SL**: TP defaults to Binance Futures ROI/PNL% via `takeProfitPercent` (`price move % = takeProfitPercent / leverage`); when `takeProfitPercent = 0.0`, TP falls back to entry ± `tp_multiplier × ATR`. SL = entry ∓ `sl_multiplier × ATR`. Time-exit = dong lenh sau `max_hold_duration` neu khong hit.

### 2.2 Assumptions

- Binance Futures dang chay o **one-way mode** (khong hedge mode). Moi symbol chi co toi da 1 position tai 1 thoi diem.
- Position tracking dung in-memory map, sync tu `AccountService::snapshot()` khi khoi dong.
- ATR duoc tinh truc tiep tu kline cache — khong can REST call them.
- WebSocket reconnect logic tai su dung `WsSession` san co.
- Strategies chay **sequential** (khong parallel) de tranh race condition tren position map.
- So luong WebSocket connections = `ceil(symbols × intervals / 1024)`.
- TP/SL duoc dat dang limit/stop order sau khi open position thanh cong.
- Kline buffer size mac dinh 200 candles — du cho indicator tinh toan (ATR 14, EMA 26, v.v.).

### 2.3 Non-Goals (khong thiet ke lan nay)

- Strategy cu the (chi thiet ke interface/framework)
- Backtesting engine
- Dashboard / UI
- Multi-account support
- BTC/ETH hedge (goi y trong strategy-all.md, de sau)
- Beta-adjusted portfolio exposure

### 2.4 Non-Functional Requirements

- **Performance**: Scan queue 2000 symbols × 2 intervals × N strategies phai chay trong thoi gian hop ly. Sequential la chap nhan duoc vi khong co blocking I/O trong buoc evaluate — I/O chi xay ra khi dat lenh.
- **Memory**: Uoc tinh 2000 symbols × 2 intervals × 200 candles × ~80 bytes/Kline ≈ **64 MB** — chap nhan duoc.
- **Reliability**: WebSocket mat ket noi → tu reconnect, cache giu nguyen, khong mat du lieu. Open positions duoc recover tu `AccountService` khi restart.
- **Correctness**: Khong mo lenh khi da co position tren cung symbol. Khong su dung lot-based sizing; chi dung notional USDT.
- **Testability**: `IStrategy` la pure interface, de mock trong unit test. `SignalEngine` nhan `IAccountService` va `IOrders` interface de test ma khong can ket noi thuc.

---

## 3. Current Project State

| Component | File | Lien quan |
|---|---|---|
| REST klines | `src/rest/rest_client.h` | `klines(symbol, interval, limit)` — dung luc init cache |
| REST exchangeInfo | `src/rest/rest_client.h` | `exchangeInfo()` — lay danh sach futures symbols |
| WebSocket kline stream | `src/ws/ws_client.h` | `subscribeKline(symbol, interval, cb)` — da co |
| WsSession | `src/transport/ws_session.h` | Auto-reconnect, multi-stream |
| Orders | `src/orders/orders.h` | `market()`, `limit()`, `cancelNormalByOrderId()` |
| AccountService | `src/account/account_service.h` | `snapshot()` — lay balance va open positions |
| Kline type | `src/types/market.h` | `struct Kline` |
| Error type | `src/types/error.h` | `BinanceError`, `Result<T>` |
| TradingEngine | `src/trading_engine.h` | Single-symbol engine hien tai — se ton tai song song, khong bi thay the |

---

## 4. Design Approaches

### 4.1 Recommended: Three-Layer Architecture

```
MarketScanner          → duy tri kline cache, push event khi closed candle
      ↓
StrategyRegistry       → quan ly danh sach IStrategy instances
      ↓
SignalEngine           → build work queue, xu ly sequential, goi Orders khi co signal
```

- **MarketScanner** doc lap voi logic trading — co the dung cho analysis tool, backtest sau nay.
- **IStrategy** la pure virtual interface — them strategy moi khong can cham vao SignalEngine.
- **SignalEngine** la orchestrator duy nhat co quyen goi Orders — giup trace rox rang.

### 4.2 Alternative: Event-driven (reject)

Moi closed candle tren WebSocket trigger immediate strategy evaluation thay vi queue.

Bi tu choi vi:

- Kho kiem soat thu tu lenh khi nhieu strategy cung fire cung luc.
- Race condition tren position map khi events den song song.
- Kho debug va replay.

### 4.3 Alternative: Parallel strategy evaluation (reject cho v1)

Chay N strategies song song tren cung (symbol, interval) de tang throughput.

Bi tu choi cho v1 vi:

- Sequential da du nhanh (evaluate khong co I/O).
- Tranh complexity cua concurrent position write.
- Co the them sau khi co test coverage tot.

---

## 5. Module Layout

```
src/
  scanner/
    market_scanner.h
    market_scanner.cpp
    kline_cache.h             # thread-safe cache type
    kline_cache.cpp

  strategy/
    istrategy.h               # pure interface + Signal type
    strategy_config.h         # StrategyConfig struct
    strategy_registry.h       # StrategyRegistry — danh sach IStrategy
    strategy_registry.cpp
    indicators/
      atr.h                   # ATR calculator (tai su dung tu existing indicator code)
      atr.cpp

  engine/
    signal_engine.h
    signal_engine.cpp
    work_queue.h              # WorkItem + queue builder
    work_queue.cpp
    position_tracker.h        # in-memory position map
    position_tracker.cpp
    sizing_policy.h           # size = max(min_notional, balance*risk/atr*sl)
    sizing_policy.cpp

tests/
  test_market_scanner.cpp
  test_strategy_interface.cpp
  test_signal_engine.cpp
  test_position_tracker.cpp
  test_sizing_policy.cpp
  test_atr_indicator.cpp
```

---

## 6. Proposed Types

### 6.1 Signal

```cpp
namespace strategy {

struct Signal {
    enum class Direction { Long, Short, None };

    Direction direction{Direction::None};
    double confidence{0.0};           // 0.0 – 1.0; engine co the filter theo nguong
    double atr{0.0};                  // ATR value tai thoi diem evaluate; 0 neu khong tinh duoc
    std::string reason;               // human-readable, dung cho log
};

} // namespace strategy
```

`atr` duoc tra ve tu strategy de engine dung tinh TP/SL va size — tranh tinh ATR nhieu lan.

### 6.2 StrategyConfig

```cpp
namespace strategy {

struct StrategyConfig {
    std::string name;

    std::vector<std::string> intervals;          // e.g. {"15m", "30m"}
    std::chrono::seconds scan_interval{3600};    // wait giua cac luot scan
    std::chrono::seconds max_hold_duration{86400}; // time-exit fallback

    double risk_pct{0.01};         // 1% of available balance per trade
    double sl_multiplier{1.5};     // SL = entry ± sl_multiplier × ATR
    double tp_multiplier{3.0};     // TP fallback = entry ± tp_multiplier × ATR
    double takeProfitPercent{20.0}; // TP default = Binance Futures ROI/PNL%
    double min_notional{1.0};      // minimum USDT per order (Binance min = 5, nhung de $1 macro)
    int    atr_period{14};         // period cho ATR calculation

    double min_confidence{0.0};    // bo qua signal co confidence thap hon nguong nay
};

} // namespace strategy
```

### 6.3 IStrategy Interface

```cpp
namespace strategy {

class IStrategy {
public:
    virtual ~IStrategy() = default;

    virtual const StrategyConfig& config() const = 0;

    // Evaluate tra ve Signal::Direction::None neu khong co tin hieu.
    // klines duoc sap xep theo thoi gian tang dan (oldest first).
    // atr trong Signal nen duoc dien neu strategy tinh ATR; neu khong engine se tu tinh.
    virtual Signal evaluate(
        std::string_view symbol,
        std::string_view interval,
        const std::vector<Kline>& klines
    ) const = 0;
};

} // namespace strategy
```

**Quy uoc**: `evaluate()` la `const` va khong co side-effects. Strategy khong duoc giu per-symbol state — state tinh toan (EMA, ATR) phai duoc tinh lai moi lan tu klines.

### 6.4 StrategyRegistry

```cpp
namespace strategy {

class StrategyRegistry {
public:
    void add(std::unique_ptr<IStrategy> strategy);

    // Tra ve view cua tat ca strategies (non-owning)
    std::vector<const IStrategy*> all() const;

    // Tra ve strategies co interval trong danh sach intervals cua no
    std::vector<const IStrategy*> forInterval(std::string_view interval) const;
};

} // namespace strategy
```

### 6.5 KlineCache

```cpp
namespace scanner {

// Key: symbol → interval → circular buffer cua Klines (oldest first)
class KlineCache {
public:
    explicit KlineCache(size_t buffer_size = 200);

    // Called khi co candle moi hoac candle dong (is_closed)
    void update(std::string_view symbol, std::string_view interval, const Kline& kline);

    // Lay snapshot (copy) de strategy evaluate — tranh shared state
    std::optional<std::vector<Kline>> snapshot(
        std::string_view symbol,
        std::string_view interval
    ) const;

    std::vector<std::string> symbols() const;
    std::vector<std::string> intervals() const;

private:
    mutable std::shared_mutex m_mutex;
    // symbol → interval → deque<Kline>
    std::unordered_map<
        std::string,
        std::unordered_map<std::string, std::deque<Kline>>
    > m_data;
    size_t m_buffer_size;
};

} // namespace scanner
```

`shared_mutex` cho phep nhieu reader (strategy evaluate) dong thoi, chi lock exclusive khi WebSocket push update.

### 6.6 MarketScanner

```cpp
namespace scanner {

class MarketScanner {
public:
    struct Config {
        std::vector<std::string> intervals{"15m", "30m"};
        size_t kline_buffer_size{200};
        size_t max_streams_per_connection{512}; // < 1024 Binance limit; du phong
    };

    MarketScanner(RestClient& rest, BinanceContext& ctx, Config config);

    // 1. Fetch exchangeInfo → lay danh sach USDT-M futures symbols
    // 2. Fetch initial klines qua REST cho moi symbol × interval (warm up cache)
    // 3. Subscribe WebSocket streams
    boost::asio::awaitable<Result<void>> start();

    void stop();

    const KlineCache& cache() const { return m_cache; }

    // Callback khi co closed candle (optional — engine co the poll cache thay the)
    using KlineClosedCb = std::function<void(std::string_view symbol, std::string_view interval)>;
    void setOnKlineClosed(KlineClosedCb cb);

private:
    // Phan chia (symbol × interval) sang nhieu WsClient instances
    boost::asio::awaitable<void> subscribeStreams(const std::vector<std::string>& symbols);

    RestClient& m_rest;
    BinanceContext& m_ctx;
    Config m_config;
    KlineCache m_cache;
    std::vector<std::unique_ptr<WsClient>> m_ws_clients;
    KlineClosedCb m_on_kline_closed;
};

} // namespace scanner
```

**Warm-up**: Khi start, MarketScanner fetch REST klines cho moi symbol × interval de fill cache ngay lap tuc. Khong can doi WebSocket tich luy du buffer.

**Multi-connection**: So luong connections = `ceil(N_symbols × N_intervals / max_streams_per_connection)`. Mac dinh 512 streams/connection → voi 2000 × 2 = 4000 streams → 8 connections.

### 6.7 WorkItem & WorkQueue

```cpp
namespace engine {

struct WorkItem {
    std::string symbol;
    std::string interval;
    const strategy::IStrategy* strategy; // non-owning
};

class WorkQueue {
public:
    // Build queue theo round-robin: voi moi strategy, lap qua tat ca symbols × intervals
    // ma strategy do care (dua vao strategy.config().intervals).
    // Thu tu: strategy 0 × all symbols → strategy 1 × all symbols → ...
    static std::vector<WorkItem> build(
        const std::vector<std::string>& symbols,
        const strategy::StrategyRegistry& registry
    );
};

} // namespace engine
```

**Round-robin semantics**: Queue duoc xay dung mot lan khi bat dau moi scan cycle. Thu tu la strategy-major (het strategy 0 cho tat ca symbols roi moi sang strategy 1), de dam bao moi strategy duoc chay day du.

### 6.8 PositionTracker

```cpp
namespace engine {

struct TrackedPosition {
    std::string symbol;
    strategy::Signal::Direction direction;
    std::chrono::system_clock::time_point opened_at;
    std::chrono::seconds max_hold_duration;
    double entry_price;
    int64_t tp_order_id{0};   // order id cua TP limit order
    int64_t sl_order_id{0};   // order id cua SL stop order
};

class PositionTracker {
public:
    // Sync tu AccountService::snapshot() khi khoi dong
    void loadFromSnapshot(const std::vector<Position>& positions);

    void add(TrackedPosition pos);
    void remove(std::string_view symbol);
    bool has(std::string_view symbol) const;

    // Tra ve cac positions da qua max_hold_duration
    std::vector<TrackedPosition> expired(std::chrono::system_clock::time_point now) const;

    std::vector<TrackedPosition> all() const;

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, TrackedPosition> m_positions;
};

} // namespace engine
```

### 6.9 SizingPolicy

```cpp
namespace engine {

struct SizingInput {
    double available_balance;  // USDT
    double atr;                // ATR cua symbol tai TF hien tai
    double risk_pct;           // e.g. 0.01 = 1%
    double sl_multiplier;      // e.g. 1.5
    double min_notional;       // e.g. 1.0 USDT
};

struct SizingResult {
    double notional;           // USDT value de giao dich
    double quantity;           // tinh theo step_size cua symbol
    bool is_min_clamped;       // true neu notional < min_notional va da clamp
};

// size = max(min_notional, balance * risk_pct / (atr * sl_multiplier))
// quantity = notional / current_price, sau do round xuong theo stepSize
SizingResult calculateSize(
    const SizingInput& input,
    double current_price,
    double step_size      // tu ExchangeInfo cua symbol
);

} // namespace engine
```

### 6.10 SignalEngine

```cpp
namespace engine {

class SignalEngine {
public:
    struct Config {
        double min_notional{1.0};
        std::chrono::seconds position_check_interval{60}; // tan suat check time-exit
    };

    SignalEngine(
        scanner::MarketScanner& scanner,
        strategy::StrategyRegistry& registry,
        AccountService& account,
        Orders& orders,
        Config config
    );

    // Chay mai mai; dung khi goi stop()
    boost::asio::awaitable<void> run();
    void stop();

private:
    // Mot scan cycle: build queue → process tung item → sleep → repeat
    boost::asio::awaitable<void> runScanCycle();

    // Evaluate mot WorkItem; neu co signal → openPosition
    boost::asio::awaitable<void> processItem(const WorkItem& item);

    // Mo lenh moi voi TP/SL
    boost::asio::awaitable<Result<void>> openPosition(
        std::string_view symbol,
        strategy::Signal::Direction direction,
        double atr,
        const strategy::StrategyConfig& cfg
    );

    // Chay song song voi runScanCycle; dong lenh het time-exit
    boost::asio::awaitable<void> monitorTimeExit();

    // Lay available_balance tu AccountService
    boost::asio::awaitable<double> fetchAvailableBalance();

    scanner::MarketScanner& m_scanner;
    strategy::StrategyRegistry& m_registry;
    AccountService& m_account;
    Orders& m_orders;
    Config m_config;
    PositionTracker m_tracker;
    std::atomic<bool> m_running{false};
};

} // namespace engine
```

---

## 7. Data Flow

### 7.1 Startup Sequence

```
main()
  ├── BinanceContext::start()
  ├── RestClient::connect()
  ├── AccountService::snapshot()      → load open positions vao PositionTracker
  ├── MarketScanner::start()
  │     ├── RestClient::exchangeInfo()  → lay danh sach symbols
  │     ├── RestClient::klines() × N   → warm up cache (co rate limit throttle)
  │     └── WsClient::subscribeKline() × connections → bat dau nhan stream
  └── SignalEngine::run()
        ├── co_spawn monitorTimeExit()
        └── loop: runScanCycle()
```

### 7.2 Scan Cycle

```
runScanCycle():
  queue = WorkQueue::build(scanner.cache().symbols(), registry)
  for each WorkItem(symbol, interval, strategy):
    if tracker.has(symbol) → skip
    klines = scanner.cache().snapshot(symbol, interval)
    if klines.empty() → skip
    signal = strategy->evaluate(symbol, interval, klines)
    if signal.direction == None → continue
    if signal.confidence < strategy.config().min_confidence → continue
    openPosition(symbol, signal.direction, signal.atr, strategy.config())

  // Sau het queue, tinh scan_interval ngan nhat trong tat ca strategies
  // de tranh wait qua lau khi co strategy can rescan som
  sleep(min_scan_interval_across_strategies)
```

### 7.3 Open Position Flow

```
openPosition(symbol, direction, atr, cfg):
  balance = fetchAvailableBalance()
  sizing  = calculateSize({balance, atr, cfg.risk_pct, cfg.sl_multiplier, cfg.min_notional},
                           current_price, step_size)
  side    = direction == Long ? Buy : Sell

  result  = orders.market(MarketOrderDraft{symbol, side, sizing.quantity, PositionSide::Both})
  if result.error → log, return

  entry_price = result.avgPrice (hoac mark price neu ACK mode)

  // Dat TP
  tp_price = direction == Long ? entry + atr*tp_mult : entry - atr*tp_mult
  tp_result = orders.limit(LimitOrderDraft{symbol, opposite_side, qty, tp_price, GTC})

  // Dat SL
  sl_price = direction == Long ? entry - atr*sl_mult : entry + atr*sl_mult
  sl_result = orders.algo stopProtection(...)

  tracker.add(TrackedPosition{symbol, direction, now(), cfg.max_hold_duration,
                               entry_price, tp_order_id, sl_order_id})
```

### 7.4 Time-Exit Flow

```
monitorTimeExit():
  loop every position_check_interval:
    expired = tracker.expired(now())
    for each pos in expired:
      orders.cancelNormalByOrderId(pos.symbol, pos.tp_order_id)
      orders.cancelAlgoByAlgoId(pos.sl_order_id)
      orders.closeByMarket(CloseByMarketDraft{pos.symbol})
      tracker.remove(pos.symbol)
```

### 7.5 TP/SL Hit Detection

Khi Binance fill TP hoac SL order, `UserDataStream` nhan `ORDER_TRADE_UPDATE` event. Engine lang nghe event nay de cap nhat `PositionTracker`:

```
UserDataStream callback:
  if event.orderStatus == "FILLED" && event.clientOrderId matches tracked tp/sl:
    tracker.remove(event.symbol)
```

---

## 8. ATR Calculator

```cpp
namespace strategy::indicators {

// Tra ve vector ATR co cung do dai voi klines (leading NaN duoc fill = 0.0).
// period thuong = 14.
std::vector<double> atr(const std::vector<Kline>& klines, int period = 14);

// Lay ATR moi nhat (phan tu cuoi cung).
// Tra ve 0.0 neu khong du data.
double lastAtr(const std::vector<Kline>& klines, int period = 14);

} // namespace strategy::indicators
```

Cong thuc:

- `TR = max(high - low, |high - prev_close|, |low - prev_close|)`
- `ATR[0] = mean(TR[0..period-1])`
- `ATR[i] = (ATR[i-1] × (period-1) + TR[i]) / period` (Wilder smoothing)

---

## 9. Configuration

Cau hinh engine duoc doc tu `config.json` (extend file hien co):

```json
{
  "scanner": {
    "intervals": ["15m", "30m"],
    "kline_buffer_size": 200,
    "max_streams_per_connection": 512
  },
  "engine": {
    "min_notional": 1.0,
    "position_check_interval_seconds": 60
  },
  "strategies": [
    {
      "name": "momentum_v1",
      "type": "momentum",
      "intervals": ["15m"],
      "scan_interval_seconds": 3600,
      "max_hold_duration_seconds": 86400,
      "risk_pct": 0.01,
      "sl_multiplier": 1.5,
      "tp_multiplier": 3.0,
      "takeProfitPercent": 20.0,
      "min_notional": 1.0,
      "atr_period": 14,
      "min_confidence": 0.5
    }
  ]
}
```

Strategy duoc load bang factory pattern:

```cpp
std::unique_ptr<IStrategy> StrategyFactory::create(const StrategyConfig& cfg, const nlohmann::json& params);
```

---

## 10. Error Handling

| Scenario | Xu ly |
|---|---|
| WebSocket disconnect | WsSession auto-reconnect; cache giu nguyen; khong clear positions |
| exchangeInfo fail khi start | `MarketScanner::start()` tra ve `Result::error`; engine khong chay |
| REST klines fail khi warm-up | Log warning, skip symbol do, tiep tuc voi cac symbol khac |
| evaluate() throw exception | Catch trong `processItem()`, log, skip item, tiep tuc queue |
| market order fail | Log BinanceError, khong them vao tracker, tiep tuc |
| TP/SL order fail sau open | Log warning; position van duoc track; time-exit se don dep |
| ATR = 0 (khong du data) | Skip mo lenh cho item do; log warning |
| balance = 0 | Skip mo lenh; log warning |

---

## 11. Testing Strategy

### Unit Tests

| Test file | Scope |
|---|---|
| `test_kline_cache.cpp` | Thread-safety (concurrent read/write), buffer rotation, snapshot correctness |
| `test_atr_indicator.cpp` | ATR computation voi known fixtures; edge case < period candles |
| `test_sizing_policy.cpp` | Formula correctness; min_notional clamp; step_size rounding |
| `test_position_tracker.cpp` | add/remove/has, expired() logic, loadFromSnapshot |
| `test_work_queue.cpp` | Queue order; round-robin assignment; filter by interval |
| `test_signal_engine.cpp` | Mock IStrategy, mock Orders, mock AccountService; verify skip-on-position, verify openPosition called correctly |
| `test_strategy_interface.cpp` | Compile-time: tao mock strategy, verify interface contract |

### Integration Tests (manual / sandbox)

- Start MarketScanner voi testnet; verify cache duoc fill sau 1-2 closed candles.
- Chay SignalEngine voi mock strategy luon tra Long signal; verify lenh duoc dat dung.
- Verify time-exit dong lenh sau `max_hold_duration`.

---

## 12. Phased Implementation Plan

### Phase A — MarketScanner + KlineCache

- `src/scanner/kline_cache.h/.cpp`
- `src/scanner/market_scanner.h/.cpp`
- Fetch exchangeInfo → warm-up REST klines → subscribe WebSocket
- Tests: `test_kline_cache.cpp`

### Phase B — IStrategy Interface + ATR Indicator

- `src/strategy/istrategy.h`
- `src/strategy/strategy_config.h`
- `src/strategy/strategy_registry.h/.cpp`
- `src/strategy/indicators/atr.h/.cpp`
- Tests: `test_atr_indicator.cpp`, `test_strategy_interface.cpp`

### Phase C — PositionTracker + SizingPolicy + WorkQueue

- `src/engine/position_tracker.h/.cpp`
- `src/engine/sizing_policy.h/.cpp`
- `src/engine/work_queue.h/.cpp`
- Tests: `test_position_tracker.cpp`, `test_sizing_policy.cpp`, `test_work_queue.cpp`

### Phase D — SignalEngine

- `src/engine/signal_engine.h/.cpp`
- Tich hop MarketScanner, StrategyRegistry, PositionTracker, Orders, AccountService
- Tests: `test_signal_engine.cpp` (voi mocks)

### Phase E — UserDataStream Integration

- Lang nghe `ORDER_TRADE_UPDATE` event de tu dong remove position khoi tracker khi TP/SL hit
- Tranh phu thuoc hoan toan vao polling

### Phase F — First Concrete Strategy

- Implement strategy dau tien (cu the se thiet ke rieng)
- Chay sandbox voi account nho, verify end-to-end flow

---

## 13. Decision Log

| Quyet dinh | Lua chon da xem xet | Ly do chon | Trang thai |
|---|---|---|---|
| Sequential work-queue thay vi event-driven | Event-driven callback khi closed candle | Tranh race condition; de debug; thu tu lenh ro rang | Approved |
| WebSocket cache thay vi REST polling | REST polling, Hybrid | REST rate limit qua thap cho 2000 symbols; WebSocket khong ton weight | Approved |
| One-way mode (skip symbol neu co position) | Hedge mode (long + short dong thoi) | Don gian hon; du cho v1; tranh complexity quan ly 2 chieu | Approved |
| ATR-based TP/SL + time-exit fallback | Fixed %, Fixed $, Chi time-exit | ATR adapt theo volatility; time-exit bao ve khi ATR TP/SL qua xa | Approved |
| Sizing = max(min_notional, balance × risk / (atr × sl_mult)) | Fixed notional, Fixed % only | Ket hop ca 3 cach: floor an toan, scale theo equity, adapt theo volatility | Approved |
| Per-strategy scan_interval + max_hold_duration | Global config | Moi strategy co nhip do rieng; flexible cho multi-strategy setup | Approved |
| Timeframes configurable (bat dau 15m + 30m) | Fixed 1 TF, Fixed 3 TF | De mo rong; khong rebuild khi them/bot TF | Approved |
| evaluate() la const, khong giu per-symbol state | Strategy giu EMA/ATR state | Tranh shared mutable state; de test; re-entrant safe | Approved |
| Warm-up cache qua REST truoc khi nhan WebSocket | Chi dung WebSocket tu dau | Tranh wait qua lau khi can du buffer (200 candles) | Approved |
| TP/SL dat bang limit/stop order rieng | Trail stop, OCO | Don gian; OCO Binance Futures co the khong available cho tat ca symbol | Approved |

---

## 14. Recommendation

Implement theo thu tu Phase A → B → C → D → E → F.

Phase A + B co the chay song song (doc lap). Phase C phu thuoc B (can `IStrategy`). Phase D phu thuoc A + B + C. Phase E co the them sau khi Phase D on dinh.

**Khong implement strategy cu the cho den Phase F** — dam bao interface on dinh truoc khi viet strategy dau tien.
