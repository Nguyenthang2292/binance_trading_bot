# Warmup Parallelization

**Version:** 1.1
**Date:** 2026-05-15
**Status:** Ready for implementation

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.1 | 2026-05-15 | Review fixes: corrected Binance kline weight boundaries, replaced passive shared limiter with reservation-based limiter, added atomic cache merge, added cancellable backfill lifecycle, included beta daily warmup, added implementation checklist and tests |
| 1.0 | 2026-05-15 | Initial design: parallel warm-up pool, shared RateLimiter, two-phase strategy |

---

## 1. Muc Tieu

Giam thoi gian blocking warm-up cua `MarketScanner` tu ~6-7 phut xuong gan sat gioi han vat ly cua Binance API va network, trong khi khong lam sai thu tu candles ma strategy doc tu `KlineCache`.

**Nguyen nhan goc re hien tai:**
- Warm-up dang chay tuan tu tren mot `RestClient`.
- Delay co dinh 100ms giua moi request lam tang thoi gian blocking bat ke Binance co gan rate limit hay khong.
- `RateLimiter` hien tai chi phan ung sau khi doc response header; no khong reserve weight truoc khi gui request nen khong du an toan cho parallel workers.
- `KlineCache::update()` chi thay duplicate neu duplicate nam o cuoi deque; backfill fetch lai full range co the lam snapshot tam thoi sai thu tu thoi gian.

**Muc tieu implement:**
- Phase 1 blocking fetch candles song song voi `warmupInitialLimit=99` mac dinh de moi `klines` request ton weight 1 theo Binance USD-M docs.
- Phase 1 gom ca regular intervals va beta daily klines neu `betaDailyKlinesEnabled=true`.
- Phase 2 backfill fetch day du den `klineBufferSize` trong background sau khi websocket subscribe xong.
- Backfill phai co cancellation/lifecycle ro rang; khong dung detached coroutine vo chu giu raw `this`.
- Moi REST client trong process dung chung mot IP-level `RateLimiter`.
- Moi update batch vao `KlineCache` phai merge theo `openTime`, dedupe, trim, va publish atomically.

**Non-goals:**
- Khong them disk persistence.
- Khong dam bao tuyet doi < 30 giay.
- Khong thay doi trading logic, strategy evaluation, order sizing, hay exposure policy.

---

## 2. Assumptions Va Constraints

### 2.1 Assumptions

| # | Assumption |
|---|---|
| A1 | Active strategies co the bat dau voi 99 candles. Neu strategy can nhieu hon, user phai tang `warmup_initial_limit`; limiter se tinh weight dung cho limit do. |
| A2 | Binance USD-M futures `GET /fapi/v1/klines` weight theo `limit`: `[1,100)` = 1, `[100,500)` = 2, `[500,1000]` = 5, `>1000` = 10. `limit=100` KHONG phai weight 1. |
| A3 | API request weight limit mac dinh la 2400/minute theo IP; limiter phai coordinate tat ca REST clients vi limit theo IP, khong theo API key. |
| A4 | RTT futures REST trung binh ~30-80ms, nhung TLS connect, proxy, parser, va scheduling co the lam phase 1 cham hon ly thuyet. |
| A5 | `io_context` hien tai co thread pool mac dinh 2; async concurrency van huu ich vi request cho network I/O phan lon thoi gian. |

### 2.2 External References

- Binance USD-M Kline docs: `https://developers.binance.com/docs/derivatives/usds-margined-futures/market-data/rest-api/Kline-Candlestick-Data`
- Binance USD-M General Info / Limits: `https://developers.binance.com/docs/derivatives/usds-margined-futures/general-info`

### 2.3 Acceptance Criteria

- `warmup_initial_limit=99`, `warmup_concurrency=10`, current scanner intervals, and current symbol universe finish regular phase materially faster than sequential warmup.
- No request burst can exceed limiter budget when N `RestClient` instances run concurrently.
- 429/418/503 responses do not cause tight retry loops.
- `KlineCache::snapshot()` always returns candles sorted ascending by `openTime`.
- `SignalEngine` can start after phase 1 and websocket subscription; backfill continues without blocking engine startup.
- `MarketScanner::stop()` cancels background backfill and prevents use-after-free.
- Existing config remains backward compatible; new fields have safe defaults.

---

## 3. Hien Trang

### 3.1 Current Flow

```text
MarketScanner::start()
  stop()
  exchangeInfo()
  for symbol in symbols:
    for interval in intervals:
      m_rest.klines(symbol, interval, klineBufferSize)
      m_cache.update(each kline)
      sleep(warmupRequestDelay)
  optional beta daily loop, also sequential
  subscribeStreams()
  return
```

### 3.2 Current Bottlenecks

Voi 1593 regular requests:

```text
1593 * (50ms RTT + 100ms delay) = ~239s minimum
```

Thuc te log ~6-7 phut do overhead ket noi, proxy/TLS jitter, parser, sequential scheduling, va rate-limit waits.

### 3.3 Correct Binance Kline Weight Matrix

| `klines` limit | Weight/request | Max requests/min at 2400 weight/min | 1593 requests, weight-only lower bound |
|---|---:|---:|---:|
| 1-99 | 1 | 2400 | ~40s |
| 100-499 | 2 | 1200 | ~80s |
| 500-1000 | 5 | 480 | ~200s |
| 1001-1500 | 10 | 240 | ~400s |

Important:
- `limit=99` is the default phase 1 value because it stays weight 1.
- `limit=100` is weight 2, same tier as `limit=200`.
- Existing `limit=200` is not weight 5, but sequential delay still dominates wall-clock time.

---

## 4. Target Architecture

### 4.1 Overview

```text
BinanceContext
  owns shared RateLimiter
  makeRestClient() injects shared RateLimiter into every RestClient

MarketScanner::start()
  stop()
  exchangeInfo()
  build regular WorkItems(limit=warmupInitialLimit)
  add beta daily WorkItems(limit=betaDailyLimit) if enabled
  co_await WarmupPool.run(workItems)
  subscribeStreams()
  startBackfillJob(symbols) if enabled and klineBufferSize > warmupInitialLimit
  return

BackfillJob
  cancellable owned background task
  fetches full regular interval ranges at low pressure
  KlineCache.merge(...) publishes sorted/deduped batches atomically
```

### 4.2 Phase 1: Blocking Parallel Warmup

Phase 1 fetches enough candles for initial strategy operation. It must complete before `start()` returns.

Work items:
- Regular intervals: every tradable USDT perpetual symbol * every configured scanner interval, limit `warmupInitialLimit`.
- Beta daily: if `betaDailyKlinesEnabled=true`, every regular symbol plus `BTCUSDT` if missing, interval `betaDailyInterval`, limit `betaDailyLimit`.

Rules:
- Use `warmupInitialLimit=99` by default.
- Clamp `warmupInitialLimit` to `[1, klineBufferSize]`.
- Compute request weight from actual limit; do not assume one fixed weight.
- If all regular warmup items fail and total > 0, return `Result<void>` error from `start()`.
- If only a subset fails, log warnings and continue with partial cache.
- Beta daily failures are logged. Existing exposure failure mode decides whether trading can continue safely.

### 4.3 Phase 2: Background Backfill

Phase 2 improves cache depth from initial candles to `klineBufferSize`.

Rules:
- Start only after `subscribeStreams()` completes.
- Do not block `SignalEngine` startup.
- Use low pressure: default `backfillConcurrency=1`, default `backfillRequestDelay=200ms`.
- Use same shared `RateLimiter` as every other REST request.
- Use `KlineCache::merge()` for batch updates; never append full historical ranges candle-by-candle.
- Backfill is cancellable and owned by `MarketScanner`; no raw detached coroutine may outlive scanner.
- `stop()` must cancel backfill, cancel pending timer if any, disconnect websockets, and make destruction safe.

---

## 5. Config Changes

### 5.1 `scanner::MarketScanner::Config`

```cpp
struct Config {
    std::vector<std::string> intervals{"15m", "30m"};
    size_t klineBufferSize{200};
    size_t maxStreamsPerConnection{512};

    // Legacy field. Keep for backwards compatibility, but phase 1 no longer
    // sleeps per request. New code should prefer backfillRequestDelay.
    std::chrono::milliseconds warmupRequestDelay{0};

    size_t warmupInitialLimit{99};
    size_t warmupConcurrency{10};

    bool backfillEnabled{true};
    size_t backfillConcurrency{1};
    std::chrono::milliseconds backfillRequestDelay{200};

    bool betaDailyKlinesEnabled{false};
    std::string betaDailyInterval{"1d"};
    int betaDailyLimit{31};
};
```

### 5.2 `config.json`

```json
"scanner": {
  "intervals": ["15m", "30m", "4h"],
  "kline_buffer_size": 200,
  "max_streams_per_connection": 512,
  "warmup_request_delay_ms": 0,
  "warmup_initial_limit": 99,
  "warmup_concurrency": 10,
  "backfill_enabled": true,
  "backfill_concurrency": 1,
  "backfill_request_delay_ms": 200
}
```

Parsing rules in `main.cpp`:
- Missing fields use `Config` defaults.
- `warmup_initial_limit=0` is invalid; clamp to 1 and log warning, or reject config.
- `warmup_initial_limit > kline_buffer_size` is clamped to `kline_buffer_size`.
- `warmup_concurrency=0` is clamped to 1.
- `backfill_concurrency=0` is clamped to 1.

---

## 6. Shared Reservation-Based RateLimiter

### 6.1 Problem

Current `RateLimiter`:
- Stores used weight from response headers.
- `waitIfNeeded()` checks only current used values.
- Does not reserve weight for in-flight requests.

This is safe enough for mostly sequential REST usage, but unsafe for parallel warmup. Ten workers can all read old header usage and send requests together before any response updates the limiter.

### 6.2 Ownership

Move IP-level limiter ownership into `BinanceContext`.

```cpp
class BinanceContext {
public:
    RestClient makeRestClient();
    std::shared_ptr<RateLimiter> rateLimiter() const { return m_rateLimiter; }

private:
    std::shared_ptr<RateLimiter> m_rateLimiter;
};
```

`makeRestClient()` must inject this pointer. Any manual `RestClient` created for warmup must receive the same pointer.

`RestClient` keeps backwards compatibility:

```cpp
class RestClient {
public:
    RestClient(boost::asio::io_context& ioc,
               boost::asio::ssl::context& ssl,
               ContextConfig cfg);

    RestClient(boost::asio::io_context& ioc,
               boost::asio::ssl::context& ssl,
               ContextConfig cfg,
               std::shared_ptr<RateLimiter> sharedRateLimiter);

private:
    std::shared_ptr<RateLimiter> m_rateLimiter;
};
```

Constructor without limiter creates a private limiter only for tests or legacy direct construction.

### 6.3 API

```cpp
class RateLimiter {
public:
    struct Limits {
        int requestWeightPerMinute = 2400;
        int ordersPerMinute = 1200;
        int ordersPer10Seconds = 300;
        double safetyRatio = 0.95;
    };

    struct Cost {
        int requestWeight = 1;
        int orders1m = 0;
        int orders10s = 0;
    };

    boost::asio::awaitable<void> acquire(Cost cost);
    void updateFromHeaders(int usedWeight, int usedOrders1m, int usedOrders10s);
    void penalize(std::chrono::milliseconds delay);

    static int klineWeight(int limit);
};
```

`klineWeight(limit)`:

```cpp
if (limit < 1) return 1;       // caller should validate; defensive fallback
if (limit < 100) return 1;
if (limit < 500) return 2;
if (limit <= 1000) return 5;
return 10;
```

### 6.4 Acquire Semantics

`acquire(cost)` must:
- Compute effective used weight as `max(headerUsedWeight, reservedWeight)`.
- Reserve `cost` before returning to caller.
- Wait until the current limiter window can safely fit the cost if budget is exhausted.
- Use `safetyRatio` for normal requests to avoid hard 429 boundaries.
- Reset local reservations when the local 1-minute or 10-second window rolls.

Pseudo-flow:

```cpp
while (true) {
    lock state;
    refreshLocalWindows(now);
    if (canFit(cost)) {
        reservedWeight += cost.requestWeight;
        reservedOrders1m += cost.orders1m;
        reservedOrders10s += cost.orders10s;
        unlock;
        co_return;
    }
    delay = timeUntilNextRelevantWindow();
    unlock;
    co_await timer(delay);
}
```

`updateFromHeaders(...)` reconciles local state with Binance response headers. Header values are authoritative for already accepted requests, but reservations remain necessary for in-flight requests.

### 6.5 RestClient Integration

Each REST method must acquire with endpoint-specific cost before sending.

For `klines()`:

```cpp
const int cost = RateLimiter::klineWeight(limit);
auto body = co_await publicGet("/fapi/v1/klines", q, RateLimiter::Cost{.requestWeight = cost});
```

`publicGet`, `signedGet`, `signedPost`, `signedPut`, and `signedDelete` should accept a `RateLimiter::Cost`.

### 6.6 Retry/Backoff

HTTP handling stays in `RestClient`/`HttpSession`, but retry policy must prevent tight loops:

| Status | Handling |
|---|---|
| 429 | Call `RateLimiter::penalize(nextWindowDelay or exponential backoff)`, retry idempotent market-data requests up to 3 attempts. Do not retry signed trade/order mutation requests blindly. |
| 418 | Treat as fatal for current call, log error, and penalize limiter for a long cooldown. |
| 503 service unavailable | Retry idempotent market-data requests with exponential backoff up to 3 attempts. |
| 503 unknown execution status | Do not blindly retry signed trade/order mutation requests. Market-data requests may retry. |

Warmup uses only public market-data requests, so retries are safe within max attempts.

---

## 7. KlineCache Batch Merge

### 7.1 Problem

`KlineCache::update()` only replaces the last candle if `bucket.back().openTime == kline.openTime`. If backfill fetches the latest 200 candles oldest-first after phase 1 already inserted the latest 99, appending each candle can create wrong chronological order in snapshots.

### 7.2 New API

Add batch merge:

```cpp
class KlineCache {
public:
    void update(std::string_view symbol, std::string_view interval, const Kline& kline);
    void merge(std::string_view symbol, std::string_view interval, std::span<const Kline> klines);
};
```

`merge()` rules:
- Acquire cache write lock once.
- Combine existing bucket and incoming klines.
- Sort ascending by `openTime`.
- Dedupe by `openTime`; keep the newest incoming value when duplicate exists.
- Trim to newest `m_bufferSize` candles.
- Publish atomically before releasing lock.

`update()` may delegate to `merge()` for one candle or keep the optimized last-candle path and fall back to merge when incoming `openTime` is not newer than the back element.

Acceptance:
- `snapshot()` always returns ascending `openTime`.
- Duplicate candles never increase bucket size.
- Closed websocket update after REST merge replaces the same candle, not appends.

---

## 8. WarmupPool

### 8.1 WorkItem

```cpp
class WarmupPool {
public:
    struct WorkItem {
        std::string symbol;
        std::string interval;
        int limit{99};
        std::string phase{"regular"};
    };

    struct Result {
        size_t completed{0};
        size_t succeeded{0};
        size_t failed{0};
    };

    WarmupPool(BinanceContext& ctx,
               size_t concurrency,
               std::shared_ptr<RateLimiter> rateLimiter);

    boost::asio::awaitable<Result> run(std::vector<WorkItem> items, KlineCache& cache);
};
```

### 8.2 Worker Logic

Implementation notes:
- Create N `RestClient` instances, all using `m_ctx.rateLimiter()`.
- Pop work under queue mutex.
- Do network request without holding queue mutex.
- Merge cache batch without holding queue mutex.
- Update counters under mutex.
- Log failures with symbol, interval, phase, limit, and reason.

```cpp
boost::asio::awaitable<void> worker(RestClient& client, KlineCache& cache, size_t id) {
    while (true) {
        WorkItem item;
        {
            std::lock_guard lock(m_queueMutex);
            if (m_queue.empty()) co_return;
            item = std::move(m_queue.back());
            m_queue.pop_back();
        }

        auto klines = co_await client.klines(item.symbol, item.interval, item.limit);
        if (klines) {
            cache.merge(item.symbol, item.interval, *klines);
            markSuccess(item);
        } else {
            markFailure(item, klines.error());
        }
    }
}
```

### 8.3 Completion

Use `boost::asio::experimental::channel` or an equivalent completion counter to await all workers. Exceptions from workers must be caught and counted as failures; they must not deadlock `run()`.

If `items.empty()`, return `{0,0,0}` immediately.

---

## 9. MarketScanner Flow

### 9.1 Build Work Items

```cpp
std::vector<WarmupPool::WorkItem> workItems;

const int regularLimit = normalizedWarmupInitialLimit();
for (const auto& symbol : symbols) {
    for (const auto& interval : m_config.intervals) {
        workItems.push_back({
            .symbol = symbol,
            .interval = interval,
            .limit = regularLimit,
            .phase = "regular",
        });
    }
}

if (m_config.betaDailyKlinesEnabled) {
    std::vector<std::string> betaSymbols = symbols;
    if (std::find(betaSymbols.begin(), betaSymbols.end(), "BTCUSDT") == betaSymbols.end()) {
        betaSymbols.push_back("BTCUSDT");
    }

    const int betaLimit = std::max(2, m_config.betaDailyLimit);
    for (const auto& symbol : betaSymbols) {
        workItems.push_back({
            .symbol = symbol,
            .interval = m_config.betaDailyInterval,
            .limit = betaLimit,
            .phase = "beta",
        });
    }
}
```

### 9.2 Start

```cpp
boost::asio::awaitable<Result<void>> MarketScanner::start() {
    stop();
    m_symbolInfo.clear();

    auto symbolsResult = co_await m_rest.exchangeInfo();
    if (!symbolsResult) {
        co_return std::unexpected(symbolsResult.error());
    }

    auto symbols = filterTradableSymbols(*symbolsResult);
    auto workItems = buildWarmupWorkItems(symbols);

    WarmupPool pool(m_ctx, m_config.warmupConcurrency, m_ctx.rateLimiter());
    auto result = co_await pool.run(std::move(workItems), m_cache);

    if (regularWarmupTotal > 0 && regularWarmupSucceeded == 0) {
        co_return std::unexpected(BinanceError::fromApiResponse(
            -91001,
            "market scanner warmup failed for all regular kline requests"));
    }

    co_await subscribeStreams(symbols);
    startBackfill(symbols);

    co_return Result<void>{};
}
```

Implementation must track regular success separately from beta success so beta failures do not mask regular warmup status.

### 9.3 Backfill Job Ownership

`MarketScanner` adds:

```cpp
class MarketScanner {
private:
    struct BackfillState {
        std::atomic_bool cancel{false};
        std::mutex mutex;
        std::shared_ptr<boost::asio::steady_timer> timer;
        std::shared_future<void> done;
    };

    void startBackfill(const std::vector<std::string>& symbols);
    void cancelBackfill();
    boost::asio::awaitable<void> backgroundBackfill(
        std::vector<std::string> symbols,
        std::shared_ptr<BackfillState> state);

    std::shared_ptr<BackfillState> m_backfillState;
};
```

Rules:
- `startBackfill()` calls `cancelBackfill()` first.
- `backgroundBackfill()` checks `state->cancel` before and after each network request and timer wait.
- Timer waits use a timer stored in `BackfillState`; `cancelBackfill()` posts timer cancellation onto the `io_context`.
- `stop()` calls `cancelBackfill()` before clearing scanner state.
- Destructor should call `stop()`.

The first implementation may allow `stop()` to wait for an in-flight HTTP request to return, because current `HttpSession` does not expose request cancellation. This is acceptable if logged; follow-up work can add request-level cancellation.

### 9.4 Backfill Logic

```cpp
boost::asio::awaitable<void> MarketScanner::backgroundBackfill(
    std::vector<std::string> symbols,
    std::shared_ptr<BackfillState> state) {
    const int fullLimit = static_cast<int>(std::max<size_t>(1, m_config.klineBufferSize));

    std::vector<WarmupPool::WorkItem> items;
    for (const auto& symbol : symbols) {
        for (const auto& interval : m_config.intervals) {
            items.push_back({
                .symbol = symbol,
                .interval = interval,
                .limit = fullLimit,
                .phase = "backfill",
            });
        }
    }

    // First implementation may run sequentially to reduce pressure.
    for (const auto& item : items) {
        if (state->cancel.load()) co_return;

        auto klines = co_await m_rest.klines(item.symbol, item.interval, item.limit);
        if (state->cancel.load()) co_return;

        if (klines) {
            m_cache.merge(item.symbol, item.interval, *klines);
        } else {
            // Log warning with symbol, interval, phase, limit, and error reason.
        }

        co_await cancellableDelay(state, m_config.backfillRequestDelay);
    }
}
```

If `backfillConcurrency > 1`, reuse `WarmupPool` with `phase="backfill"` and low concurrency. Default remains 1 to avoid competing with trading REST calls.

---

## 10. Time Estimates

Assume 1593 regular requests and 50ms average RTT.

| Scenario | Limit | Weight/request | Delay | Concurrency | Expected wall time |
|---|---:|---:|---:|---:|---:|
| Current sequential | 200 | 2 | 100ms | 1 | ~4-7 min observed |
| Sequential no delay | 99 | 1 | 0ms | 1 | ~80s network-bound |
| Phase 1 parallel | 99 | 1 | 0ms | 10 | ~15-45s typical |
| Phase 1 with limiter waits / high RTT | 99 | 1 | 0ms | 10 | ~40-70s acceptable |
| Full blocking warmup without phase split | 200 | 2 | 0ms | 10 | >=80s weight-bound |
| Background backfill | 200 | 2 | 200ms | 1 | ~5-8 min, non-blocking |

Notes:
- Weight-only lower bound for 1593 requests at weight 1 is ~40s if requests must be spread evenly over a full minute, but phase 1 total weight 1593 is below 95% of 2400, so it can often complete in one limiter window.
- Actual Binance behavior, proxy, TLS setup, and local CPU can move this number materially.

---

## 11. Error Handling

| Situation | Handling |
|---|---|
| Single warmup request network/parse failure | Log warning, count failure, continue. |
| All regular warmup requests fail | Return error from `MarketScanner::start()`. |
| Partial regular warmup failures | Continue after logging summary. Strategy for missing symbol/interval simply has no snapshot until websocket/backfill fills it. |
| Beta daily failures | Log warning. Exposure failure mode handles missing beta data. |
| 429 during market-data request | Penalize limiter, retry with bounded backoff. |
| 418 IP ban | Log error, fail current call, long limiter cooldown. |
| 503 market-data request | Retry with bounded exponential backoff. |
| Backfill stop requested | Set cancel token, cancel timer, stop after current awaitable returns. |
| Cache duplicate/openTime overlap | `KlineCache::merge()` sorts, dedupes, trims atomically. |

---

## 12. Files To Change

| File | Change |
|---|---|
| `src/context.h` | Add shared `std::shared_ptr<RateLimiter>` member/accessor. |
| `src/context.cpp` | Initialize limiter and inject it in `makeRestClient()`. |
| `src/rest/rate_limiter.h` | Add `Cost`, `acquire()`, `updateFromHeaders()`, `penalize()`, and `klineWeight()`. |
| `src/rest/rate_limiter.cpp` | Implement reservation-based limiter and window reset logic. |
| `src/rest/rest_client.h` | Store `std::shared_ptr<RateLimiter>` and add constructor overload. |
| `src/rest/rest_client.cpp` | Use shared limiter, acquire endpoint-specific costs, retry safe market-data requests. |
| `src/scanner/kline_cache.h` | Add `merge()` batch API. |
| `src/scanner/kline_cache.cpp` | Implement sorted/deduped atomic merge. |
| `src/scanner/market_scanner.h` | Add config fields and backfill state/methods. |
| `src/scanner/market_scanner.cpp` | Implement `WarmupPool`, phase 1 work queue, beta work items, cancellable backfill. |
| `src/main.cpp` | Parse new scanner config fields with validation/clamping. |
| `config.json` | Add new scanner fields and default `warmup_initial_limit=99`. |
| `tests/test_rate_limiter.cpp` | Add limiter unit tests. |
| `tests/test_kline_cache.cpp` | Add merge ordering/dedupe tests. |
| `tests/test_market_scanner.cpp` or existing scanner tests | Add warmup work-item construction/backfill lifecycle tests if practical. |

---

## 13. Test Plan

### 13.1 Unit Tests

`RateLimiter::klineWeight()`:
- `1 -> 1`
- `99 -> 1`
- `100 -> 2`
- `499 -> 2`
- `500 -> 5`
- `1000 -> 5`
- `1001 -> 10`
- `1500 -> 10`

`RateLimiter::acquire()`:
- Concurrent acquire calls reserve budget before response headers arrive.
- Calls wait when reserved + cost exceeds safety budget.
- Header update reconciles local usage without dropping in-flight reservations.
- `penalize()` forces subsequent acquire to wait.

`KlineCache::merge()`:
- Incoming sorted range inserts normally.
- Incoming overlapping range dedupes by `openTime`.
- Incoming older full range after latest partial range still returns ascending snapshot.
- Bucket trims to newest `m_bufferSize`.
- `update()` of same last candle still replaces, not appends.

### 13.2 Integration/Behavior Tests

Market scanner:
- Build regular work items count = symbols * intervals.
- Build beta work items includes `BTCUSDT` exactly once when beta enabled.
- All regular failures return error from `start()`.
- Partial failures log summary and allow `start()` to continue.
- `stop()` cancels an active backfill timer.

Manual run:
- Start bot on testnet or read-only live keys.
- Verify warmup logs include `requests`, `concurrency`, `initial_limit`, `success`, `failed`, elapsed seconds.
- Verify no 429/418 appears during normal startup.
- Verify snapshots are sorted by `openTime` after backfill completes.

---

## 14. Decision Log

| # | Decision | Alternatives Rejected | Reason |
|---|---|---|---|
| D1 | Use `warmupInitialLimit=99` by default | `100` or `200` | Binance weight boundary is `[1,100)` for weight 1; `100` is weight 2. |
| D2 | Centralize shared limiter in `BinanceContext` | Create ad hoc shared limiter only for WarmupPool | Binance limit is IP-level; backfill and trading REST calls also need coordination. |
| D3 | Add reservation-based `RateLimiter::acquire(cost)` | Keep current response-header-only limiter | Parallel workers need pre-send budget reservation. |
| D4 | Add `KlineCache::merge()` | Backfill with repeated `update()` | Full-range backfill overlaps phase 1 and can reorder snapshots unless merged atomically. |
| D5 | Include beta daily klines in phase 1 work queue | Leave beta daily sequential | Current beta loop can preserve the old bottleneck when exposure is enabled. |
| D6 | Own cancellable backfill task | `boost::asio::detached` capturing raw `this` | Detached raw-member coroutine can outlive scanner and cause use-after-free. |
| D7 | Default backfill concurrency to 1 | Parallel backfill by default | Backfill is non-blocking and should not compete aggressively with trading REST calls. |
| D8 | Fail startup only if all regular warmup fails | Fail on any request failure | Partial market-data misses are recoverable through websocket/backfill; all-fail indicates startup is not useful. |

---

## 15. Implementation Order

1. Implement and test `RateLimiter::klineWeight()`.
2. Implement reservation-based `RateLimiter::acquire()` and update existing REST calls to use `Cost`.
3. Move shared limiter ownership to `BinanceContext`; update `RestClient` constructors.
4. Implement `KlineCache::merge()` and tests.
5. Implement `WarmupPool` and replace regular/beta sequential loops in `MarketScanner::start()`.
6. Add config parsing and logging.
7. Implement cancellable backfill lifecycle.
8. Run unit tests.
9. Run a read-only startup smoke test and inspect logs for elapsed time, failures, limiter waits, and cache ordering.
