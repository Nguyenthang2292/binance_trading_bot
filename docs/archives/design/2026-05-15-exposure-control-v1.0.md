# Exposure Control Layer — Design Document

**Version:** 1.1
**Date:** 2026-05-15
**Status:** ✅ DONE - Implemented

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-05-15 | Initial design: BetaCalculator, ExposureController, hybrid soft/hard limit, KlineCache BTCUSDT 1d extension, SignalEngine integration |
| 1.1 | 2026-05-15 | Implementation readiness fixes: daily beta cache for benchmark + symbols, absolute gross exposure with signed beta support, sizing-before-check flow, includePositions snapshot, configurable fail-open/fail-closed behavior |

---

## 1. Mục Tiêu

Thiết kế lớp `ExposureController` inject vào `SignalEngine::openPosition()` để kiểm soát **beta-adjusted portfolio exposure** trước khi mở lệnh mới. Mục tiêu là đảm bảo portfolio duy trì trạng thái market-neutral (hoặc theo target cấu hình) theo notional × beta, thay vì chỉ đếm số lệnh LONG/SHORT.

**Vấn đề hiện tại:**

- `SignalEngine::processItem()` gọi `openPosition()` ngay khi có signal hợp lệ, không kiểm tra portfolio-level exposure.
- `PositionTracker` chỉ theo dõi per-symbol, không tính tổng beta exposure.
- `StrategyConfig` không có cấu hình `maxLong`, `maxShort`, hay `targetLongShortRatio`.
- LONG/SHORT chỉ cân bằng nếu signals tình cờ bằng nhau — không được enforce.

**Phạm vi thiết kế lần này:**

1. `BetaCalculator` — tính beta của coin so với BTC dùng OLS regression trên 30 ngày daily returns.
2. `ExposureController` — kiểm tra và ra quyết định Allow / ScaleDown / Block trước mỗi lần mở lệnh.
3. Extend `MarketScanner` warm-up/subscription để hold daily kline data cho BTCUSDT benchmark và từng tradable symbol cần tính beta.
4. Integrate vào `SignalEngine::openPosition()` qua port injection pattern (nhất quán với IScannerPort, IAccountPort, IOrdersPort đã có).
5. Config section `exposure_control` trong `config.json`.

---

## 2. Understanding Lock

### 2.1 Summary

- **Cái gì được xây dựng**: Lớp `ExposureController` inject vào `SignalEngine::openPosition()` để kiểm soát portfolio exposure trước khi mở lệnh.
- **Tại sao**: Bot hiện tại không enforce balance — LONG/SHORT chỉ cân bằng nếu signals tình cờ bằng nhau; cần kiểm soát systematic risk theo beta để đạt market-neutral.
- **Beta = gì**: `cov(coin_daily_returns, btc_daily_returns) / var(btc_daily_returns)` — OLS regression, rolling 30 ngày daily, benchmark là BTCUSDT.
- **Hành vi khi imbalanced**: Hybrid — scale down size tuyến tính trong vùng soft limit; hard block khi vượt hard limit.
- **Không phải mục tiêu**: Không đếm số lệnh LONG/SHORT, không hardcode bất kỳ ngưỡng nào vào code.

### 2.2 Assumptions

| # | Assumption |
|---|-----------|
| A1 | `KlineCache` giữ thêm daily slot `("BTCUSDT", "1d")` và `("<symbol>", "1d")` cho các tradable symbols; buffer tối thiểu `betaWindowDays + 1` candles; được warm-up qua REST khi startup và refresh qua WebSocket closed 1d candle |
| A2 | Beta được tính tại thời điểm `openPosition()` được gọi, cached per-symbol với TTL 24h |
| A3 | Khi thiếu daily kline data cho symbol hoặc benchmark BTCUSDT, dùng `default_beta` (cấu hình, mặc định 1.0) và log warning một lần per symbol |
| A4 | Portfolio notional per position = `quantity × markPrice` từ `AccountSnapshot::positions` (match theo symbol) |
| A5 | Net beta exposure = Σ(direction_sign_i × notional_i × beta_i), đơn vị USDT. Gross beta exposure = Σ(abs(notional_i × beta_i)) |
| A6 | Config limits là fraction của `availableBalance` tại thời điểm check (scale theo account size) |
| A7 | Sau khi scale down, nếu notional < `minNotionalAfterScale` → skip hoàn toàn và log |
| A8 | Config section `exposure_control` mới trong `config.json`, không thay đổi `StrategyConfig` |
| A9 | `ExposureController` được inject vào `SignalEngine` qua interface `IExposurePort` (nhất quán với port pattern hiện có) |
| A10 | Failure mode là cấu hình: mặc định `closed` để bảo toàn risk guarantee; có thể đổi sang `open` nếu muốn ưu tiên availability |

### 2.3 Non-Goals

- Không tính correlation matrix giữa các coins với nhau.
- Không tự động rebalance portfolio (đóng positions cũ để cân bằng).
- Không tính factor exposure ngoài beta (Fama-French, sector, v.v.).
- Không tracking drawdown hay portfolio volatility.
- Không có beta-weighted sizing (chỉ dùng exposure để gate, sizing vẫn theo ATR/risk_pct hiện tại).

### 2.4 Non-Functional Requirements

- **Performance**: `check()` phải chạy trong < 1ms (synchronous, chỉ đọc cache + tính toán số học). Không có I/O trong hot path.
- **Thread safety**: `ExposureController` là const sau construction. Beta cache có mutex riêng.
- **Testability**: `IExposurePort` là pure interface, dễ mock trong `test_signal_engine.cpp`.
- **Reliability**: Nếu `ExposureController` throw exception, `openPosition()` catch và áp dụng `failureMode`: mặc định fail-closed/block lệnh; có thể cấu hình fail-open nếu muốn ưu tiên availability.

---

## 3. Current Project State

| Component | File | Liên quan |
|---|---|---|
| SignalEngine | `src/engine/signal_engine.h:59` | `openPosition()` là điểm inject |
| PositionTracker | `src/engine/position_tracker.h` | `all()` cung cấp danh sách open positions |
| KlineCache | `src/scanner/kline_cache.h` | Đã hỗ trợ arbitrary `(symbol, interval)`; dùng để hold daily beta candles cho BTCUSDT và symbols |
| AccountSnapshot | `src/types/account.h` | `Position::notional` — position size thực tế |
| SizingPolicy | `src/engine/sizing_policy.h` | `SizingResult::notional` là proposedNotional cho exposure check; khi ScaleDown phải recompute `quantity` theo `stepSize` |
| IScannerPort | `src/engine/signal_engine.h:26` | Pattern để thiết kế `IExposurePort` |
| Signal::Direction | `src/strategy/istrategy.h:12` | Long/Short/None |

---

## 4. Design Approaches

### 4.1 Recommended: ExposureController qua IExposurePort (chọn)

Thiết kế `ExposureController` implement `IExposurePort`, inject vào `SignalEngine` constructor giống các port khác (`IScannerPort`, `IAccountPort`, `IOrdersPort`). `SignalEngine::openPosition()` gọi `calculateSize()` trước để có proposed notional thực tế, rồi gọi `m_exposure.check()` trước khi gửi order.

**Ưu điểm:**
- Nhất quán với port pattern đã có — không tạo pattern mới.
- Dễ mock trong test: `MockExposurePort` chỉ cần implement `check()`.
- `ExposureController` hoàn toàn độc lập với `SignalEngine` — có thể test riêng.

**Nhược điểm:**
- Thêm 1 dependency vào `SignalEngine` constructor — cần update wiring trong `main.cpp`.

### 4.2 Alternative: Inject trực tiếp vào processItem() (reject)

Gate ở `processItem()` thay vì `openPosition()` — block sớm hơn, không compute sizing cho lệnh bị block.

Bị từ chối vì `processItem()` không có thông tin `proposedNotional` để tính deviation chính xác. Gate tại `openPosition()` mới có đủ context (balance, atr, cfg).

### 4.3 Alternative: Global portfolio monitor loop riêng (reject)

Chạy background coroutine kiểm tra exposure định kỳ và cancel positions nếu vượt limit.

Bị từ chối vì:
- Cancel position đang chạy là destructive — cần approval riêng.
- Khó test và debug hơn gate tại entry point.
- Không phù hợp với yêu cầu hiện tại (chỉ cần gate lúc mở lệnh mới).

---

## 5. Module Layout

```
src/
  engine/
    signal_engine.h            # Thêm IExposurePort& vào constructor
    signal_engine.cpp          # Gọi m_exposure.check() trong openPosition()
    exposure_controller.h      # IExposurePort interface + ExposureController + ExposureConfig
    exposure_controller.cpp    # Implementation: beta lookup, metrics, decision
    beta_calculator.h          # BetaCalculator: OLS regression trên daily returns
    beta_calculator.cpp

tests/
  test_exposure_controller.cpp # Unit tests cho ExposureController logic
  test_beta_calculator.cpp     # Unit tests cho OLS beta calculation
  test_signal_engine.cpp       # Extend với MockExposurePort
```

---

## 6. Proposed Types

### 6.1 ExposureConfig

```cpp
namespace engine {

enum class ExposureFailureMode {
    Closed,  // Block khi exposure check lỗi
    Open     // Allow full size khi exposure check lỗi
};

struct ExposureConfig {
    bool enabled{true};

    // Target net beta exposure = targetNetBeta × availableBalance (USDT)
    // 0.0 = market-neutral; positive = net long bias; negative = net short bias
    double targetNetBeta{0.0};

    // Scale down bắt đầu khi |net_beta_deviation| > softLimitNetBeta × availableBalance
    double softLimitNetBeta{0.5};

    // Hard block khi |net_beta_deviation| > hardLimitNetBeta × availableBalance
    double hardLimitNetBeta{1.0};

    // Hard block khi gross_beta_exposure > maxGrossBeta × availableBalance
    // Gross = Σ(abs(notional × beta)) cho mọi open position
    double maxGrossBeta{3.0};

    // Beta dùng khi không có daily kline data cho symbol
    double defaultBeta{1.0};

    // Sau khi scale down, skip nếu notional < minNotionalAfterScale (USDT)
    double minNotionalAfterScale{5.0};

    // Số ngày daily klines để tính beta
    int betaWindowDays{30};

    // Risk guarantee mặc định: nếu controller lỗi thì không mở lệnh.
    ExposureFailureMode failureMode{ExposureFailureMode::Closed};
};

} // namespace engine
```

### 6.2 ExposureMetrics

```cpp
namespace engine {

struct ExposureMetrics {
    double longBetaExposure{0.0};   // Σ(notional_i × beta_i) cho Long positions; có thể âm nếu beta âm
    double shortBetaExposure{0.0};  // Σ(notional_i × beta_i) cho Short positions; có thể âm nếu beta âm
    double netBetaExposure{0.0};    // longBeta − shortBeta; tương đương Σ(direction_sign_i × notional_i × beta_i)
    double grossBetaExposure{0.0};  // Σ(abs(notional_i × beta_i)); luôn không âm
    int positionCount{0};
};

} // namespace engine
```

### 6.3 ExposureDecision & ExposureCheckResult

```cpp
namespace engine {

enum class ExposureDecision {
    Allow,      // Mở lệnh với full size
    ScaleDown,  // Mở lệnh nhưng nhân notional với scaleFactor
    Block       // Từ chối mở lệnh hoàn toàn
};

struct ExposureCheckResult {
    ExposureDecision decision{ExposureDecision::Allow};
    double scaleFactor{1.0};   // [0, 1] — chỉ có nghĩa khi decision == ScaleDown
    std::string reason;        // Human-readable, dùng cho log
};

} // namespace engine
```

### 6.4 IExposurePort Interface

```cpp
namespace engine {

class IExposurePort {
public:
    virtual ~IExposurePort() = default;

    // Kiểm tra exposure trước khi mở lệnh.
    // proposedNotional: sizing.notional trước khi apply scaleFactor (USDT).
    // Caller phải nhân sizing.notional × result.scaleFactor sau khi gọi.
    virtual ExposureCheckResult check(
        std::string_view symbol,
        strategy::Signal::Direction direction,
        double proposedNotional,
        const PositionTracker& tracker,
        const account::AccountSnapshot& snapshot,
        double availableBalance
    ) const = 0;

    // Metrics hiện tại — dùng cho logging/monitoring, không dùng trong hot path
    virtual ExposureMetrics currentMetrics(
        const PositionTracker& tracker,
        const account::AccountSnapshot& snapshot,
        double availableBalance
    ) const = 0;

    // Dùng bởi SignalEngine khi check() throw exception.
    virtual ExposureFailureMode failureMode() const = 0;

    // Dùng bởi SignalEngine khi áp dụng ScaleDown.
    virtual double minNotionalAfterScale() const = 0;
};

// No-op implementation — dùng khi exposure_control.enabled = false
class NoOpExposurePort final : public IExposurePort {
public:
    ExposureCheckResult check(
        std::string_view, strategy::Signal::Direction,
        double, const PositionTracker&,
        const account::AccountSnapshot&, double) const override
    {
        return {ExposureDecision::Allow, 1.0, "exposure control disabled"};
    }

    ExposureMetrics currentMetrics(
        const PositionTracker&,
        const account::AccountSnapshot&, double) const override
    {
        return {};
    }

    ExposureFailureMode failureMode() const override {
        return ExposureFailureMode::Open;
    }

    double minNotionalAfterScale() const override {
        return 0.0;
    }
};

} // namespace engine
```

### 6.5 BetaCalculator

```cpp
namespace engine {

class BetaCalculator {
public:
    // Tính beta của symbol vs BTCUSDT dùng OLS trên daily returns.
    // Đọc klines từ cache: snapshot(symbol, "1d") và snapshot("BTCUSDT", "1d").
    // Trả về std::nullopt nếu không đủ data (< 2 candles).
    std::optional<double> calculate(
        std::string_view symbol,
        const scanner::KlineCache& cache,
        int windowDays
    ) const;

private:
    // Tính daily returns từ close prices: r_t = (close_t - close_{t-1}) / close_{t-1}
    static std::vector<double> toReturns(const std::vector<Kline>& klines);

    // OLS: beta = cov(y, x) / var(x)
    // x = btc_returns, y = coin_returns
    static double ols(
        const std::vector<double>& coinReturns,
        const std::vector<double>& btcReturns
    );
};

} // namespace engine
```

**Công thức OLS:**

```
mu_x = mean(btc_returns)
mu_y = mean(coin_returns)
cov  = Σ((x_i - mu_x) × (y_i - mu_y)) / (n - 1)
var  = Σ((x_i - mu_x)^2) / (n - 1)
beta = cov / var
```

Nếu `var(btc_returns) ≈ 0` (BTC không biến động trong window) → return `std::nullopt`.

### 6.6 ExposureController

```cpp
namespace engine {

class ExposureController final : public IExposurePort {
public:
    ExposureController(ExposureConfig config, const scanner::KlineCache& cache);

    ExposureCheckResult check(
        std::string_view symbol,
        strategy::Signal::Direction direction,
        double proposedNotional,
        const PositionTracker& tracker,
        const account::AccountSnapshot& snapshot,
        double availableBalance
    ) const override;

    ExposureMetrics currentMetrics(
        const PositionTracker& tracker,
        const account::AccountSnapshot& snapshot,
        double availableBalance
    ) const override;

    ExposureFailureMode failureMode() const override {
        return m_config.failureMode;
    }

    double minNotionalAfterScale() const override {
        return m_config.minNotionalAfterScale;
    }

private:
    // Tra về beta của symbol, dùng m_betaCache nếu hit.
    // Nếu miss: tính qua BetaCalculator, cache kết quả.
    // Nếu không có data: trả về m_config.defaultBeta.
    double getBeta(std::string_view symbol) const;

    // Lấy current notional của symbol từ AccountSnapshot.
    // Fallback: TrackedPosition::quantity × TrackedPosition::entryPrice nếu không có trong snapshot.
    static double getPositionNotional(
        std::string_view symbol,
        const account::AccountSnapshot& snapshot,
        const TrackedPosition& pos
    );

    // Tính portfolio metrics từ danh sách positions hiện tại.
    ExposureMetrics computeMetrics(
        const std::vector<TrackedPosition>& positions,
        const account::AccountSnapshot& snapshot,
        double availableBalance
    ) const;

    // Tính ExposureDecision dựa trên deviation so với target.
    ExposureCheckResult decide(
        double currentNetBeta,
        double directionSign,
        double proposedNotional,
        double beta,
        double softLimit,
        double hardLimit,
        double grossBetaExposure,
        double maxGross
    ) const;

    ExposureConfig m_config;
    const scanner::KlineCache& m_cache;
    BetaCalculator m_betaCalc;

    // Beta cache: symbol → (beta, computed_at)
    mutable std::unordered_map<
        std::string,
        std::pair<double, std::chrono::system_clock::time_point>
    > m_betaCache;
    mutable std::mutex m_betaCacheMutex;

    static constexpr std::chrono::hours kBetaCacheTTL{24};
};

} // namespace engine
```

---

## 7. Data Flow

### 7.1 Startup Sequence (Extension)

```
Existing startup flow:
  └── MarketScanner::start()
        ├── RestClient::exchangeInfo()       → symbols list
        ├── RestClient::klines() × N         → warm up cache (15m, 30m)
        ├── WsClient::subscribeKline() × N   → start streams
        └── [NEW] when exposure_control.enabled:
              ├── RestClient::klines(symbol, "1d", betaWindowDays + 1)
              │     for BTCUSDT benchmark and each tradable symbol
              │     → cache.update(symbol, "1d", ...)
              └── WsClient::subscribeKline(symbol, "1d")
                    for BTCUSDT benchmark and each tradable symbol
                                             → ready for BetaCalculator
```

Refresh daily beta data: `MarketScanner` subscribe thêm stream `@kline_1d` cho benchmark và symbol universe. Mỗi khi closed candle 1d → `cache.update(symbol, "1d", kline)`. Không cần polling riêng.

### 7.2 openPosition() Flow (Updated)

```
openPosition(symbol, direction, atr, currentPrice, cfg):

  [UPDATED] snapshotReq.includePositions = true
  [UPDATED] snapshot = co_await m_account.snapshot(snapshotReq)
  [UPDATED] balance = snapshot.account.availableBalance

  [EXISTING] sizing = calculateSize({balance, atr, cfg.riskPct,
                                     cfg.slMultiplier, max(cfg.minNotional, engine.minNotional)},
                                    currentPrice, stepSize)
  if sizing.quantity <= 0:
      co_return error("quantity is zero after sizing")

  [NEW] ── Exposure Check, using actual pre-scale sizing ───────────
  try:
      expResult = m_exposure.check(
          symbol, direction, sizing.notional,
          m_tracker, snapshot, balance)
  catch:
      if m_exposure.failureMode() == ExposureFailureMode::Closed:
          log error and co_return {}
      expResult = Allow(full size)

  if expResult.decision == Block:
      spdlog::warn("[Exposure] Blocked {} {}: {}", symbol, dir, expResult.reason)
      co_return {}
  ─────────────────────────────────────────────────────────────────

  [NEW] ── Apply Scale Factor ─────────────────────────────────────
  if expResult.decision == ScaleDown:
      scaledNotional = sizing.notional * expResult.scaleFactor
      if scaledNotional < m_exposure.minNotionalAfterScale():
          spdlog::warn("[Exposure] ScaleDown → notional too small, skipping {} {}", ...)
          co_return {}
      sizing.quantity = floor(scaledNotional / currentPrice / stepSize) * stepSize
      sizing.notional = sizing.quantity * currentPrice
      if sizing.quantity <= 0:
          spdlog::warn("[Exposure] ScaleDown → quantity rounded to zero, skipping {} {}", ...)
          co_return {}
      if sizing.notional < m_exposure.minNotionalAfterScale():
          spdlog::warn("[Exposure] ScaleDown → notional too small, skipping {} {}", ...)
          co_return {}
  ─────────────────────────────────────────────────────────────────

  [EXISTING] orders.market(...)  → open position
  [EXISTING] orders.limit(...)   → TP
  [EXISTING] orders.protection(...)  → SL
  [EXISTING] m_tracker.add(...)
```

### 7.3 ExposureController::check() Logic

```
check(symbol, direction, proposedNotional, tracker, snapshot, balance):

  softLimit = config.softLimitNetBeta × balance
  hardLimit = config.hardLimitNetBeta × balance
  maxGross  = config.maxGrossBeta × balance
  target    = config.targetNetBeta × balance

  metrics   = computeMetrics(tracker.all(), snapshot, balance)
  beta      = getBeta(symbol)

  dirSign   = direction == Long ? +1.0 : -1.0

  // Tính deviation sau khi thêm lệnh mới với full size
  currentDeviation = |metrics.netBetaExposure - target|
  newNet = metrics.netBetaExposure + dirSign × proposedNotional × beta
  newDeviation = |newNet - target|
  proposedGross = abs(proposedNotional × beta)
  newGross = metrics.grossBetaExposure + proposedGross

  // Hard gross check (cả hai chiều)
  if newGross > maxGross:
      return Block("gross beta exposure {newGross:.0f} > max {maxGross:.0f}")

  // Nếu lệnh mới làm net exposure gần target hơn, allow full size.
  // Điều này cho phép SHORT mới giảm net-long imbalance, hoặc LONG mới giảm net-short imbalance.
  if newDeviation <= currentDeviation:
      return Allow("improves net beta deviation")

  // Net deviation checks
  if newDeviation >= hardLimit:
      return Block("net beta deviation {newDeviation:.0f} >= hard limit {hardLimit:.0f}")

  if newDeviation >= softLimit:
      // Scale factor tuyến tính: 1.0 tại soft limit → 0.0 tại hard limit
      scaleFactor = (hardLimit - newDeviation) / (hardLimit - softLimit)
      scaleFactor = clamp(scaleFactor, 0.0, 1.0)
      return ScaleDown(scaleFactor, "net beta deviation {newDeviation:.0f}, scale {scaleFactor:.2f}")

  return Allow()
```

---

## 8. Daily Beta Kline Cache Extension

`KlineCache` không cần thay đổi interface — nó đã hỗ trợ bất kỳ `(symbol, interval)` nào.

**Thay đổi trong `MarketScanner`:** thêm daily beta warm-up/subscription khi exposure control bật.

```cpp
// Config fields proposed:
bool betaDailyKlinesEnabled{false};
std::string betaDailyInterval{"1d"};
int betaDailyLimit{31}; // betaWindowDays + 1

// 1. Warm-up daily klines for benchmark + all tradable symbols.
std::vector<std::string> betaSymbols = symbols;
if (std::find(betaSymbols.begin(), betaSymbols.end(), "BTCUSDT") == betaSymbols.end()) {
    betaSymbols.push_back("BTCUSDT");
}

for (const auto& symbol : betaSymbols) {
    auto daily = co_await m_rest.klines(symbol, m_config.betaDailyInterval, m_config.betaDailyLimit);
    if (!daily) {
        Logger::instance().log(LogLevel::Warning,
            "market_scanner beta daily warmup failed symbol=" + symbol +
            " reason=" + daily.error().toString());
        continue;
    }
    for (const auto& k : *daily) {
        m_cache.update(symbol, m_config.betaDailyInterval, k);
    }
}

// 2. Subscribe 1d kline stream for benchmark + symbols.
// Reuse existing stream chunking by maxStreamsPerConnection.
```

**Lý do không extend `KlineCache` struct:** Buffer size mặc định 200 candles >> 31 candles cần cho beta, không cần special-case.

**Quan trọng:** chỉ warm-up `BTCUSDT 1d` là không đủ. `BetaCalculator::calculate(symbol, cache, windowDays)` cần cả `snapshot(symbol, "1d")` và `snapshot("BTCUSDT", "1d")`. Nếu thiếu daily data cho alt symbol thì beta sẽ fallback về `defaultBeta`, làm exposure control kém chính xác.

---

## 9. SignalEngine Integration

**Constructor thay đổi:**

```cpp
// Trước
SignalEngine(
    IScannerPort& scanner,
    strategy::StrategyRegistry& registry,
    IAccountPort& account,
    IOrdersPort& orders,
    Config config);

// Sau
SignalEngine(
    IScannerPort& scanner,
    strategy::StrategyRegistry& registry,
    IAccountPort& account,
    IOrdersPort& orders,
    IExposurePort& exposure,   // [NEW]
    Config config);
```

**Member mới:**

```cpp
IExposurePort& m_exposure;
```

**Wiring trong `main.cpp`:**

```cpp
// Khi exposure_control.enabled = true:
ExposureController exposureCtrl(exposureConfig, scanner.cache());
SignalEngine engine(scanner, registry, account, orders, exposureCtrl, engineConfig);

// Khi exposure_control.enabled = false:
NoOpExposurePort noOpExposure;
SignalEngine engine(scanner, registry, account, orders, noOpExposure, engineConfig);
```

---

## 10. Configuration

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
  "exposure_control": {
    "enabled": true,
    "target_net_beta": 0.0,
    "soft_limit_net_beta": 0.5,
    "hard_limit_net_beta": 1.0,
    "max_gross_beta": 3.0,
    "default_beta": 1.0,
    "min_notional_after_scale": 5.0,
    "beta_window_days": 30,
    "failure_mode": "closed"
  },
  "strategies": [...]
}
```

**Ý nghĩa các tham số (ví dụ với balance = $10,000):**

| Tham số | Giá trị | Ý nghĩa |
|---|---|---|
| `target_net_beta` | 0.0 | Muốn net beta exposure = $0 (market-neutral) |
| `soft_limit_net_beta` | 0.5 | Scale down khi net deviation > $5,000 |
| `hard_limit_net_beta` | 1.0 | Block khi net deviation > $10,000 |
| `max_gross_beta` | 3.0 | Block khi total beta exposure > $30,000 |
| `default_beta` | 1.0 | Dùng beta = 1.0 nếu thiếu data |
| `min_notional_after_scale` | 5.0 | Skip lệnh nếu sau scale < $5 |
| `failure_mode` | `closed` | Nếu exposure check lỗi thì block lệnh. Có thể đổi `open` để allow full size |

---

## 11. Error Handling

| Scenario | Xử lý |
|---|---|
| BTCUSDT 1d fetch fail khi startup | Log warning, tiếp tục. `getBeta()` dùng `defaultBeta` cho các symbols cho tới khi benchmark data có lại |
| Symbol thiếu daily kline data | `getBeta()` trả về `defaultBeta`, log warning một lần per symbol |
| `var(btc_returns) ≈ 0` (BTC không move) | `BetaCalculator::calculate()` trả `nullopt` → dùng `defaultBeta` |
| ExposureController throw exception | `openPosition()` catch, log error, áp dụng `failure_mode`: `closed` → block lệnh; `open` → tiếp tục full size |
| AccountSnapshot không có position của symbol | Fallback: dùng `quantity × entryPrice` từ `TrackedPosition` |
| `scaleFactor` tính ra âm (deviation > hardLimit nhưng logic bị lỗi) | `clamp(scaleFactor, 0.0, 1.0)` bảo vệ; log error |

---

## 12. Testing Strategy

### Unit Tests

| Test file | Scope |
|---|---|
| `test_beta_calculator.cpp` | OLS formula với known fixtures; edge case n=0, n=1; var≈0; negative beta (inverse assets) |
| `test_exposure_controller.cpp` | Allow khi deviation nhỏ; Allow khi lệnh mới cải thiện net deviation; ScaleDown với scaleFactor đúng; Block khi vượt hardLimit; Block khi vượt maxGross; gross dùng `abs(notional × beta)` với negative beta; defaultBeta fallback; scaleFactor clamp [0,1] |
| `test_signal_engine.cpp` | Extend với `MockExposurePort` — verify Block → openPosition không gọi orders; verify ScaleDown → quantity giảm và rounded theo stepSize; verify Allow → full size; verify exposure exception dùng `failure_mode` đúng; verify snapshot includePositions được request |

### Test Fixtures cho BetaCalculator

```cpp
// BTC tăng 1% mỗi ngày, coin tăng 2% mỗi ngày → beta ≈ 2.0
// BTC tăng 1%, coin giảm 1% → beta ≈ -1.0
// BTC không đổi (var=0) → nullopt
// BTC và coin cùng returns → beta ≈ 1.0
```

### Integration Test (sandbox)

- Mở nhiều LONG positions → verify bot scale down / block LONG mới, vẫn allow SHORT.
- Verify metrics log sau mỗi scan cycle.

---

## 13. Phased Implementation Plan

### Phase A — BetaCalculator

- `src/engine/beta_calculator.h/.cpp`
- Update `CMakeLists.txt` `LIB_SOURCES` / `LIB_HEADERS`
- Tests: `test_beta_calculator.cpp`
- Không dependency mới, có thể implement độc lập.

### Phase B — ExposureController + IExposurePort

- `src/engine/exposure_controller.h/.cpp`
- `NoOpExposurePort` trong cùng file
- Update `CMakeLists.txt` `LIB_SOURCES` / `LIB_HEADERS`
- Tests: `test_exposure_controller.cpp` (mock KlineCache)

### Phase C — KlineCache / MarketScanner Extension

- `MarketScanner::start()`: thêm daily 1d warm-up và stream subscription cho BTCUSDT benchmark + tradable symbols khi exposure control bật
- Tests: verify `cache.snapshot("BTCUSDT", "1d")` và ít nhất một tradable alt symbol có data sau start

### Phase D — SignalEngine Integration

- Update `signal_engine.h/.cpp`: thêm `IExposurePort&` parameter, fetch snapshot `{includePositions=true}`, gọi `check()` trong `openPosition()` sau `calculateSize()`, áp dụng `ScaleDown` lên quantity/notional đã rounded
- Update `main.cpp` wiring
- Update `test_signal_engine.cpp` với `MockExposurePort`

---

## 14. Decision Log

| Quyết định | Lựa chọn đã xem xét | Lý do chọn | Trạng thái |
|---|---|---|---|
| Beta benchmark = BTC | Market index, ETH, per-strategy | BTC là dominant factor trong crypto; dữ liệu luôn available; đơn giản | Approved |
| Rolling window = 30 ngày daily | Strategy interval window, configurable per strategy | Fixed window độc lập strategy, stable hơn; reuse KlineCache với slot riêng | Approved |
| Data source = daily beta candles in KlineCache for benchmark + symbols | BTCUSDT-only daily cache, Separate BetaCache class, REST on-demand | `BetaCalculator` cần cả coin returns và benchmark returns; reuse infrastructure đã có; không thêm class mới; MarketScanner đã quản lý WebSocket | Approved |
| Hybrid soft/hard limit | Hard block only, Scale down only | Soft limit giữ bot hoạt động liên tục; hard limit là lưới an toàn cuối; industry standard | Approved |
| Limits = fraction × availableBalance | Absolute USDT, fraction of totalNotional | Scale tự động theo account size; nhất quán với `risk_pct` pattern trong StrategyConfig | Approved |
| Fully configurable — tất cả tham số trong config.json | Hardcode target=0 và limits | Market regime thay đổi; không cần recompile; nhất quán với thiết kế tổng thể | Approved |
| IExposurePort injection | Singleton, direct instantiation | Nhất quán với IScannerPort/IAccountPort/IOrdersPort; dễ mock trong test | Approved |
| Configurable failure mode, default fail-closed | Always fail-open, Always fail-closed | Default fail-closed giữ risk guarantee; fail-open vẫn có thể bật khi ưu tiên availability | Approved |
| defaultBeta = 1.0 khi thiếu data | 0.0, nullopt → skip, configurable | Beta 1.0 = assume correlated với market (conservative assumption); configurable để user override | Approved |
| ScaleDown áp dụng sau calculateSize và step rounding lại quantity | Scale balance trước khi sizing, Check exposure trước sizing | Tách biệt concern: sizing logic không bị thay đổi; exposure check dùng notional thật trước scale; quantity sau scale vẫn hợp lệ với stepSize | Approved |
| Chỉ gate khi MỞ lệnh, không tự rebalance | Auto-close positions khi vượt limit | Destructive action (đóng lệnh) cần approval riêng; scope hiện tại chỉ là gate entry | Approved |
