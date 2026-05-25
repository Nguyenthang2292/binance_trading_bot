# REST-Backed Historical Window Provider for Backtest Gate v1.0

**Status:** Draft
**Date:** 2026-05-24
**Owner:** Backtest gate
**Related:** [2026-05-23-backtest-parameter-optimizer-v1.1.md](2026-05-23-backtest-parameter-optimizer-v1.1.md)

**Revision history (in-place):**

- 2026-05-24: v1.0 initial.
- 2026-05-24: v1.0-rev1 — targeted REST client (use existing `RestClient` with `startTime/endTime`, drop `BinanceAPI` extension); page size 1500 (Binance USD-M Futures actual cap); off-by-one corrected in fetch window math; `WindowResult` extended with observability metadata; integration test gated by env-var `GTEST_SKIP()`; async/sync bridge documented.

---

## 1. Context & Problem

The backtest gate currently supports two values for `backtest_gate.data.history_source`:

| Value | Status |
|-------|--------|
| `cache_only` | Implemented — reads from `scanner::KlineCache` only. Requires the scanner buffer to be sized ≥ `window_max_candles` or the gate is disabled at startup. |
| `cache_then_rest` | **NOT implemented in v1.1.** [`main.cpp:1258`](../../src/main.cpp) disables the gate at startup; [`historical_window_provider.cpp:63`](../../src/backtest/historical_window_provider.cpp) logs a warning and silently degrades to `cache_only`. |

Consequence: operators who want a large evaluation window (default `window_max_candles=1500`, but real deployments often want 2000+) must either oversize the scanner cache (memory pressure on every symbol/interval) or accept that the gate is permanently disabled.

**Goal:** implement a real REST-backed provider that fetches missing data from Binance when the cache is insufficient, so `cache_then_rest` becomes a working mode.

---

## 2. Understanding Summary

- Implement a REST-backed historical window provider that activates when the cache cannot satisfy `closedWindow(symbol, interval, requiredClosedBars, signalBarOpenTime)`.
- When triggered, fetch the **entire** window `[T − (N−1)·Δ, T]` (N bars whose `openTime` ends at the signal bar T) from Binance via REST; paginate when `N > 1500` bars.
- Pagination is bounded by `runtime_rest_fetch_timeout_seconds` (wall-clock) and `max_rest_requests_per_signal` (request count).
- On any pagination error or timeout → fail-closed (return `sufficient=false` so the gate drops the signal with `InsufficientData`).
- On success, write the fetched bars back to `scanner::KlineCache` (best-effort) so subsequent signals on the same `(symbol, interval)` reuse them.
- Share the **live `RestClient` instance** (created from `BinanceContext` in `main.cpp`) — `RestClient::klines` already accepts optional `startTime`/`endTime`, so no API extension is required. The shared instance preserves rate-limit, session, and proxy/auth uniformity.
- Bridge async → sync at the provider boundary: `BacktestGateController::evaluate()` is sync but already runs on a `boost::asio::thread_pool` worker (see [signal_engine.cpp:434](../../src/engine/signal_engine.cpp)), so blocking-await via `co_spawn(ioc, …, use_future).get()` is safe (does not block the asio I/O thread).
- Extend `IHistoricalWindowProvider::WindowResult` with observability metadata (`source`, `restPagesUsed`, `restWallTimeMs`, `errorReason`) so the controller can emit structured events without the provider depending on `logStructured`.
- Startup validation: when `history_source == "cache_then_rest"`, require `runtime_rest_fetch_enabled=true` and a non-null REST client instead of disabling the gate.

## 3. Assumptions

1. Binance USD-M Futures REST `/fapi/v1/klines` accepts `endTime` (ms epoch) and `limit ≤ 1500`. Confirmed indirectly by [`RateLimiter::klineWeight`](../../src/rest/rate_limiter.cpp) which returns weight=10 for `limit > 1000` (the standard tier covering 1001–1500).
2. The `RestClient` constructed in [`main.cpp:870`](../../src/main.cpp) (`BinanceContext ctx; RestClient rest = ctx.makeRestClient();`) is available at the construction site of `BacktestGateController` and can be passed by reference. Some plumbing is needed to thread `rest` (and `ctx.ioc()`) into the gate factory.
3. Binance may return the in-progress bar when `endTime` falls inside an open interval — the provider must filter `isClosed==true && openTime <= signalTimeMs` regardless.
4. The maximum reasonable `requiredClosedBars` is ≤ 4500 bars (≈3 paginated requests at limit=1500). Default `window_max_candles=1500` → **1 request**. The existing `max_rest_requests_per_signal=3` therefore comfortably covers reasonable future windows up to 4500.
5. `RestClient` is safe to call concurrently from multiple worker threads as long as each call is dispatched onto `ctx.ioc()` via `co_spawn` (the underlying `HttpSession`/`RateLimiter` are designed for that pattern — same as live scanner usage). No additional mutex is required at the provider layer.
6. `scanner::KlineCache::merge` is already thread-safe (`shared_mutex`) — write-back is safe but bounded by `m_bufferSize` (older bars trimmed if more arrive than buffer holds; acceptable for cache_then_rest mode).

## 4. Non-Goals

- No long-term REST result cache outside `KlineCache` (no separate on-disk cache, no per-provider LRU).
- No support for `history_source` values other than `cache_only` and `cache_then_rest`.
- No gap-fill or tail-only fetch optimisations in v1.0 — always refetch the full window when cache is insufficient.
- No change to live trading data flow; the new path serves only `IHistoricalWindowProvider::closedWindow()` called by `BacktestGateController`.
- No deadline-aware adaptive pagination (each page uses a static slice of the total timeout). May revisit in v1.1 if profiling shows it matters.

## 5. Decision Log

| # | Decision | Alternatives | Rationale |
|---|----------|-------------|-----------|
| 1 | REST backfill full window when cache is insufficient | Gap-fill; cache-first + tail-backfill | Simplest, deterministic; full-window correctness is more important than minimising request count at this scale. |
| 2 | **Use the existing `RestClient::klines(symbol, interval, limit, startTime, endTime)`** as-is | Extend `BinanceAPI::getKlines`; raw HTTP in provider | `RestClient` already supports the needed signature; `BinanceAPI` is a legacy wrapper that constructs its own `BinanceContext`/`RestClient` ([binance_api.cpp:14](../../src/binance_api.cpp)) so reusing it would *not* share the live session/rate-limit. Targeting `RestClient&` honours Decision #6. |
| 3 | Automatic pagination, bounded by `max_rest_requests_per_signal` | Single request fail-on-overflow; deadline-aware | Default window (1500) fits in **one** request; pagination is reserved for future larger windows up to 4500. |
| 4 | Fail-closed on partial pagination failure | Best-effort with partial data; retry-then-fail | Backtest results on a partial window would silently bias the gate. |
| 5 | Write back into `scanner::KlineCache::merge` | No write-back; internal memoize | Amortises REST cost across signals on the same `(symbol, interval)`. Reuses existing thread-safe API. |
| 6 | **Share live `RestClient&`** (the one created in `main.cpp`) via DI | New `BinanceAPI&`; token-bucket inside provider; ignore rate-limit | Rate-limit is naturally shared with scanner/live trading; no duplicate session, signer, proxy config; no new throttling code. |
| 7 | Decorator/chain over single-class extension | All-in-one branch in `HistoricalWindowProvider` | SRP, easier unit testing, leaves cache provider's existing tests untouched. |
| 8 | Small `IKlineRestClient` interface implemented by `RestClientKlineAdapter` (over `RestClient&` + `io_context&`) | Provider depends directly on `RestClient` | Lets unit tests inject a fake REST client without pulling Boost.Asio/SSL into the test binary; integration test uses the real adapter against the live `RestClient`. |
| 9 | **Async-to-sync bridge in the adapter** via `boost::asio::co_spawn(ioc, …, use_future).get()` with a wall-clock timeout race | Make `closedWindow` itself awaitable; offload to a worker future | `IBacktestGatePort::evaluate` is sync; controller is already on a `boost::asio::thread_pool` worker (see [signal_engine.cpp:434](../../src/engine/signal_engine.cpp)) — blocking-await there does not stall the asio I/O thread. Mirrors the pattern in [`BinanceAPI::getKlines`](../../src/binance_api.cpp). |
| 10 | **Extend `WindowResult` with observability metadata** (`source`, `restPagesUsed`, `restWallTimeMs`, `errorReason`) | Make `logStructured` a shared header; provider-level logging | Keeps logging concern in the controller (which already owns `logStructured`); provider stays pure; minimal interface change. |

## 6. Design — Approach A (Decorator/Chain)

### 6.1 Component diagram

```
                    BacktestGateController
                            │
                            ▼
                  IHistoricalWindowProvider
                            ▲
        ┌───────────────────┴───────────────────────┐
        │                                           │
HistoricalWindowProvider          RestBackfillingHistoricalWindowProvider
  (cache-only, unchanged)               │
                                        │ wraps (inner)
                                        ├──> HistoricalWindowProvider
                                        │
                                        ├──> IKlineRestClient
                                        │       ▲
                                        │       │ implements
                                        │   RestClientKlineAdapter
                                        │       │   ├──> RestClient&        (shared live instance)
                                        │       │   └──> boost::asio::io_context&  (ctx.ioc())
                                        │
                                        └──> scanner::KlineCache&  (for merge write-back)
```

### 6.2 New file layout

| File | Purpose |
|------|---------|
| `src/backtest/ikline_rest_client.h` | Pure-virtual interface used by the REST-backfilling provider (sync signature). |
| `src/backtest/rest_client_kline_adapter.h/.cpp` | Adapter that implements `IKlineRestClient` over `RestClient&` + `io_context&`. Owns pagination + async-to-sync bridge. |
| `src/backtest/rest_backfilling_historical_window_provider.h/.cpp` | New decorator provider. |
| `tests/test_rest_backfilling_historical_window_provider.cpp` | Unit tests with mock `IKlineRestClient`. |
| `tests/test_rest_client_kline_adapter.cpp` | Adapter unit tests with a fake `RestClient` seam (no network). |
| `tests/test_rest_client_kline_adapter_integration.cpp` | Integration test against the real Binance REST API, gated by env var `BINANCE_INTEGRATION_TEST=1` via `GTEST_SKIP()`. |

### 6.3 `IKlineRestClient` interface

```cpp
namespace backtest {

class IKlineRestClient {
public:
    virtual ~IKlineRestClient() = default;

    struct FetchResult {
        bool success{false};                  // false on any error/timeout
        std::vector<Kline> bars;              // ascending by openTime, may be empty
        int pagesUsed{0};                     // number of REST round-trips actually issued
        std::chrono::milliseconds wallTime{}; // total time the call took
        std::string errorMessage;             // for logging
    };

    // Fetch N=limit closed bars whose openTimes are {signalOpenMs - (limit-1)*intervalMs, ..., signalOpenMs}.
    // `signalOpenMs` is the open-time of bar T (NOT its close-time). Implementations paginate internally if
    // limit > Binance's single-request cap (1500 for /fapi/v1/klines).
    virtual FetchResult fetchClosedKlines(
        std::string_view symbol,
        std::string_view interval,
        long long signalOpenMs,
        int limit,
        std::chrono::milliseconds timeout,
        int maxRequests) = 0;
};

}  // namespace backtest
```

### 6.4 `RestClientKlineAdapter` algorithm

```text
constexpr int BINANCE_KLINE_PAGE_MAX = 1500;     // USD-M Futures /fapi/v1/klines hard cap

fetchClosedKlines(symbol, interval, signalOpenMs, limit, timeout, maxRequests):
    intervalMs   = parse_interval(interval)               // e.g. "30m" -> 1_800_000
    pageSize     = min(BINANCE_KLINE_PAGE_MAX, limit)
    pagesNeeded  = ceil(limit / pageSize)
    if pagesNeeded > maxRequests:
        return {success=false, error="budget_exceeded need=N max=M"}

    startWall = steady_clock::now()
    accumulated = []                                       // ascending by openTime once sorted
    // Anchor endTime so Binance includes the signal bar T but not bar T+1.
    cursorEnd = signalOpenMs + intervalMs - 1              // last ms before next bar opens

    for page in 1..pagesNeeded:
        wallLeft = timeout - (steady_clock::now() - startWall)
        if wallLeft <= 0ms: return {success=false, error="timeout"}

        thisPageLimit = min(pageSize, limit - len(accumulated))

        // Async->sync bridge; per-page deadline = remaining wall budget (race with steady_timer).
        Result<vector<Kline>> r = co_spawn_blocking(
            ioc,
            klines_with_timeout(restClient, symbol, interval, thisPageLimit,
                                /*startTime*/ nullopt, /*endTime*/ cursorEnd,
                                wallLeft));
        if !r: return {success=false, error="rest_failed page=" + N + " " + r.error()}

        bars = *r                                           // Binance returns ascending
        if bars.empty(): return {success=false, error="empty_page page=" + N}

        prepend(accumulated, bars)
        cursorEnd = bars.front().openTime - 1               // next page ends 1 ms before oldest we got
        if len(accumulated) >= limit: break

    // Filter: closed only, openTime <= signalOpenMs, dedupe by openTime, sort ascending
    filtered = sort_unique_by_open_time(
        filter(accumulated, b -> b.isClosed && b.openTime <= signalOpenMs))
    return {success=true, bars=filtered,
            pagesUsed=page, wallTime=steady_clock::now()-startWall}
```

**Async-to-sync helper (sketch):**

```cpp
template <class Awaitable>
auto co_spawn_blocking(boost::asio::io_context& ioc, Awaitable a) {
    auto fut = boost::asio::co_spawn(ioc, std::move(a), boost::asio::use_future);
    return fut.get();   // safe — we are on a thread_pool worker, NOT on ioc's run() thread.
}

// klines_with_timeout races RestClient::klines against a steady_timer.
// Implemented with boost::asio::experimental::awaitable_operators (`operator||`).
```

### 6.5 `RestBackfillingHistoricalWindowProvider::closedWindow`

```text
closedWindow(symbol, interval, requiredClosedBars, signalBarOpenTime):
    // 1. Try inner cache provider first (zero REST overhead path)
    inner = m_inner.closedWindow(symbol, interval, requiredClosedBars, signalBarOpenTime)
    if inner.sufficient:
        inner.source = "cache"
        return inner

    // 2. REST backfill — note signalOpenMs is the OPEN time of bar T.
    signalOpenMs = to_ms(signalBarOpenTime)
    fetch = m_restClient.fetchClosedKlines(
        symbol, interval, signalOpenMs, requiredClosedBars,
        timeout=m_cfg.runtimeRestFetchTimeoutSeconds * 1s,
        maxRequests=m_cfg.maxRestRequestsPerSignal)

    WindowResult out;
    out.requiredBars   = requiredClosedBars;
    out.restPagesUsed  = fetch.pagesUsed;
    out.restWallTimeMs = fetch.wallTime;
    out.source         = "rest";

    if !fetch.success:
        out.sufficient   = false;
        out.availableBars = inner.availableBars;
        out.errorReason   = fetch.errorMessage;
        return out;

    // 3. Validate: last bar must be exactly the signal bar T (no off-by-one).
    bars = fetch.bars                                   // already filtered & sorted by adapter
    out.availableBars = bars.size()
    if bars.size() < requiredClosedBars or bars.back().openTime != signalOpenMs:
        out.sufficient  = false
        out.errorReason = "signal_bar_missing"
        return out

    // 4. Write-back into cache (best-effort; merge is thread-safe & bounded by buffer size)
    try { m_cache.merge(symbol, interval, std::span<const Kline>(bars)); }
    catch (...) { out.errorReason = "cache_writeback_failed"; /* still return success below */ }

    // 5. Return last requiredClosedBars
    out.sufficient   = true
    out.closedKlines = tail(bars, requiredClosedBars)
    return out
```

### 6.5a `WindowResult` extension (observability)

```cpp
struct WindowResult {
    bool sufficient{false};
    int requiredBars{0};
    int availableBars{0};
    std::vector<Kline> closedKlines;

    // New (v1.0-rev1): observability metadata for structured logging by the controller.
    std::string source{};                       // "cache" | "rest" | "" (unset for legacy callers)
    int restPagesUsed{0};                       // 0 when source != "rest"
    std::chrono::milliseconds restWallTimeMs{}; // 0 when source != "rest"
    std::string errorReason{};                  // empty when sufficient or when only inner cache used
};
```

Existing callers (cache-only provider, controller) ignore the new fields → backward-compatible. Controller reads these in `evaluate()` and forwards into existing `logStructured()` calls — no provider→logger coupling.

### 6.6 No `BinanceAPI` extension — reuse `RestClient::klines`

`RestClient::klines` already exposes exactly what we need (see [rest_client.h:40-44](../../src/rest/rest_client.h)):

```cpp
boost::asio::awaitable<Result<std::vector<Kline>>> klines(
    std::string symbol,
    std::string interval,
    int limit = 500,
    std::optional<int64_t> startTime = {},
    std::optional<int64_t> endTime = {});
```

The adapter therefore only needs to:

1. Hold `RestClient&` and `boost::asio::io_context&` (both come from the live `BinanceContext` in `main.cpp`).
2. Call `klines(symbol, interval, limit, /*startTime*/ std::nullopt, endTime=cursorEnd)` per page.
3. Race each call against a `boost::asio::steady_timer` for per-page timeout, using `awaitable_operators::operator||`.
4. Bridge async→sync via `co_spawn(ioc, ..., use_future).get()` — safe because we are on a `thread_pool` worker thread, not the asio I/O thread.

`BinanceAPI::getKlines` is **not** touched. The legacy wrapper continues to exist for non-gate callers; the gate path bypasses it to avoid the duplicate `BinanceContext` cost (see [binance_api.cpp:14](../../src/binance_api.cpp)).

### 6.7 `main.cpp` factory changes (sketch)

Around line 1258 (validation):

```cpp
} else if (backtestGateConfig.data.historySource == "cache_then_rest") {
    if (!backtestGateConfig.data.runtimeRestFetchEnabled) {
        log(Error, "cache_then_rest requires runtime_rest_fetch_enabled=true; disabling gate.");
        disableBacktestGateAtStartup = true;
    }
    // else: factory below will create RestBackfillingHistoricalWindowProvider.
    // The `RestClient` and `io_context` are guaranteed to exist (constructed at line 870/871).
}
```

At provider construction (the gate factory must receive `rest` and `ctx.ioc()` already in scope from the live setup):

```cpp
auto innerCacheProvider = std::make_unique<HistoricalWindowProvider>(klineCache, dataCfg);
std::unique_ptr<IHistoricalWindowProvider> finalProvider;

if (dataCfg.historySource == "cache_then_rest") {
    auto adapter = std::make_unique<RestClientKlineAdapter>(rest, ctx.ioc());
    finalProvider = std::make_unique<RestBackfillingHistoricalWindowProvider>(
        std::move(innerCacheProvider), std::move(adapter), klineCache, dataCfg);
} else {
    finalProvider = std::move(innerCacheProvider);
}
```

(`BacktestGateController` already holds `IHistoricalWindowProvider&`; pass through `*finalProvider`.) The gate factory signature gains `RestClient& rest, boost::asio::io_context& ioc` parameters; `BacktestGateController` itself does **not** — only the factory uses them to build the adapter.

### 6.8 Error handling & logging

The provider does **not** call `logStructured` directly. It populates `WindowResult.{source, restPagesUsed, restWallTimeMs, errorReason}`; the controller maps those fields to existing structured events.

| `source` | `sufficient` | `errorReason` (provider) | Controller emits | Log level |
|----------|--------------|--------------------------|------------------|-----------|
| `cache` | `true`  | `""` | `BACKTEST_GATE_DATA_READY` (existing) | Debug/Info |
| `rest`  | `true`  | `""` or `"cache_writeback_failed"` | `BACKTEST_GATE_REST_BACKFILL_OK` (+ `cache_writeback_failed=true` if set) | Info / Warning |
| `rest`  | `false` | `"timeout"` | `BACKTEST_GATE_REST_TIMEOUT` | Warning |
| `rest`  | `false` | `"rest_failed page=N …"` | `BACKTEST_GATE_REST_ERROR` | Warning |
| `rest`  | `false` | `"budget_exceeded …"` | `BACKTEST_GATE_REST_BUDGET_EXCEEDED` | Warning |
| `rest`  | `false` | `"empty_page …"` | `BACKTEST_GATE_REST_ERROR` | Warning |
| `rest`  | `false` | `"signal_bar_missing"` | `BACKTEST_GATE_REST_SIGNAL_BAR_MISSING` | Warning |

All events carry `symbol`, `strategy_id`, `interval`, plus `pages_used`, `wall_time_ms`, `available_bars`, `required_bars` as available. Uses the existing `logStructured()` helper inside `backtest_gate_controller.cpp` — **no** new shared utility needed.

### 6.9 Edge cases

- **`requiredClosedBars == 0`** → inner returns immediately; REST path not entered.
- **Signal bar is currently open (not yet closed)** → REST may not yet have the bar; filter step drops it; provider returns `sufficient=false` with `errorReason="signal_bar_missing"`. Acceptable — gate retries on next scan tick when bar closes.
- **Interval parse failure** (`parse_interval("???")`) → adapter returns `success=false` with `error="unknown_interval"`. Should never happen — interval comes from validated config.
- **`endTime` anchoring**: `cursorEnd = signalOpenMs + intervalMs - 1` guarantees Binance includes bar T (`openTime=signalOpenMs`) but excludes bar T+1. Defensive filter `openTime <= signalOpenMs && isClosed` catches API edge cases.
- **Concurrent signals on same `(symbol, interval, signalOpenMs)`** → two workers may both REST-fetch the same window. `KlineCache::merge` deduplicates by `openTime`; both write-backs are safe; both callers get correct data. Slight wasted REST traffic is acceptable; deduping would require an in-flight map keyed by `(symbol, interval, signalOpenMs)` — deferred to v1.1 if metrics show duplicate fetch rate is high.
- **`signalBarOpenTime` is far in the past** (historical replay) → Binance `/fapi/v1/klines` supports historical `endTime`; works without change.
- **`io_context` not running** → if the live `ctx.ioc()` is not being run by the existing thread (e.g. shutdown sequence), `co_spawn(...).get()` would deadlock. Provider asserts `ioc` is supplied; the gate factory only constructs the adapter when the live context is up. This must be sequenced: construct the gate **after** scanner/REST client are alive.

## 7. Testing Strategy

### 7.1 Unit tests (`tests/test_rest_backfilling_historical_window_provider.cpp`)

Use a mock `IKlineRestClient`. Real `scanner::KlineCache` (lightweight). All tests deterministic, run in CI.

Cases:

1. Inner cache sufficient → REST not called (verify via mock call count == 0).
2. Inner insufficient, REST returns exact window → `sufficient=true`, last bar openTime == signal.
3. Inner insufficient, REST returns success but last bar ≠ signal → `sufficient=false`.
4. Inner insufficient, REST returns fewer bars than required → `sufficient=false`.
5. REST returns `success=false` (timeout/error) → `sufficient=false`, no cache mutation.
6. REST success with `isClosed=false` bars mixed in → filter drops them; if remaining < required → `sufficient=false`.
7. REST success → `KlineCache::merge` called once with all bars (verify via cache snapshot after call).
8. `requiredClosedBars=0` → inner returns sufficient empty; REST not called.
9. Mock client invoked with correct `endTimeMs`, `limit`, `timeout`, `maxRequests`.

### 7.2 Adapter unit tests (`tests/test_rest_client_kline_adapter.cpp`, no network)

Use a fake `RestClient` seam. Two practical options (pick at implementation time): (a) introduce a thin `IKlineFetcher` interface that `RestClient` satisfies via an inline adapter and tests inject a fake; (b) point the adapter at a local Boost.Beast HTTP server bound to `127.0.0.1:0` that returns canned JSON.

Cases:

1. `limit=500` → single call, `endTime = signalOpenMs + intervalMs - 1`, returns ascending bars; `pagesUsed=1`.
2. `limit=1500` → still a single call (default cap is 1500); `pagesUsed=1`.
3. `limit=3000` → 2 paginated calls, second `endTime = first.front().openTime - 1`; `pagesUsed=2`.
4. `limit=5000` with `maxRequests=3` → returns `success=false, error="budget_exceeded"` **before** any call.
5. Second page returns HTTP error → `success=false, error="rest_failed page=2 …"`, no partial bars.
6. Per-page `awaitable_operators::or` timer wins → `success=false, error="timeout"`; `wallTime ≈ timeout`.
7. Mixed `isClosed=false` in returned page → adapter filters them out before returning; if remainder < `limit` → caller sees insufficient.

### 7.3 Integration test (Q-A confirmed)

`tests/test_rest_client_kline_adapter_integration.cpp`. Network-gated **inside the test body**:

```cpp
TEST(RestClientKlineAdapterIntegration, FetchBtcUsdt30mTo1500) {
    if (std::getenv("BINANCE_INTEGRATION_TEST") == nullptr) {
        GTEST_SKIP() << "Set BINANCE_INTEGRATION_TEST=1 to run live network test.";
    }
    // ... real BinanceContext + RestClient + adapter ...
}
```

This works under MSVC multi-config (Debug/Release) — **no** custom CMake `CONFIGURATIONS` label is required, so the test is part of the normal `ctest` build target but skips by default. CI may set `BINANCE_INTEGRATION_TEST=1` on a dedicated nightly job.

Cases:

1. Fetch BTCUSDT 30m, limit=10, signalOpenMs=most recent closed 30m bar → returns 10 closed bars, monotonic openTime stride = 1_800_000 ms, last `openTime == signalOpenMs`.
2. Fetch BTCUSDT 30m, limit=3000 → returns 3000 bars across 2 pages, no gaps, `pagesUsed=2`.
3. Fetch with historical `signalOpenMs` (7 days ago) → returns bars ending at that exact open time.

### 7.4 Startup / factory tests

Extend `tests/test_backtest_gate_controller.cpp` (or a new `test_backtest_gate_startup.cpp`) to verify:

1. `history_source=cache_then_rest` with `runtime_rest_fetch_enabled=false` → gate disabled at startup.
2. `history_source=cache_then_rest` with all prerequisites → gate enabled, controller holds the decorator provider.

## 8. Open Questions / Risks

- **R1 — `RestClient` concurrency:** confirmed safe to call concurrently as long as each call is dispatched via `co_spawn(ioc, ...)`. The underlying `HttpSession` and `RateLimiter` are already used this way by the live scanner. No mutex needed at the adapter.
- **R2 — `KlineCache` buffer sizing under `cache_then_rest`:** if operator sets cache buffer = 200 but window = 1500, every signal triggers a full REST refetch (write-back is futile beyond the latest 200 bars). Acceptable in v1.0; document in operator runbook. v1.1 may relax cache size constraints once REST path is healthy.
- **R3 — Per-page wall-clock budget:** the adapter passes the **remaining** wall-clock budget to each page (not a fixed `timeout / pages` slice). For default window 1500 there is only 1 page so this is moot; the model matters once `limit > 1500`. Acceptable for v1.0 since `max_rest_requests_per_signal=3` caps total pages.
- **R4 — Duplicate concurrent fetches:** if two workers ask for the same `(symbol, interval, signalOpenMs)`, both will REST. v1.1 candidate: in-flight dedupe map keyed by that triple.
- **R5 — `io_context` lifecycle coupling:** the adapter holds `io_context&` so the gate must be torn down *before* `BinanceContext` stops its `io_context`. Wire shutdown order in `main.cpp`: stop scanner → stop gate → stop ctx.
- **R6 — Async-to-sync bridge correctness:** `co_spawn(...).get()` blocks the calling thread until the coroutine completes. This is only safe because the caller is a `boost::asio::thread_pool` worker provided by `SignalEngine::m_backtestEvaluationPool`. Document this invariant in the adapter header so future refactors don't accidentally call from the asio I/O thread.

## 9. Rollout Plan

1. Extend `IHistoricalWindowProvider::WindowResult` with `source`/`restPagesUsed`/`restWallTimeMs`/`errorReason` (additive; existing cache provider compiles & passes unchanged).
2. Add `IKlineRestClient` + `RestClientKlineAdapter` (over `RestClient&` + `io_context&`) and its unit tests with a fake-fetcher seam.
3. Add `RestBackfillingHistoricalWindowProvider` + unit tests (mock client).
4. Wire factory in `main.cpp`: thread `rest`/`ctx.ioc()` into the gate factory; remove "disable gate" branch for `cache_then_rest`; sequence shutdown (gate stops before `BinanceContext`).
5. Extend controller `evaluate()` log path to read the new `WindowResult` fields and emit the events in §6.8.
6. Update [2026-05-23-backtest-parameter-optimizer-v1.1.md](2026-05-23-backtest-parameter-optimizer-v1.1.md) to reflect that `cache_then_rest` is implemented.
7. Run integration test manually against Binance once (`BINANCE_INTEGRATION_TEST=1 ctest -R RestClientKlineAdapterIntegration`); document the command in the test header.
8. Ship behind config — operators must explicitly set `history_source=cache_then_rest` and `runtime_rest_fetch_enabled=true`.

## 10. References

- [src/backtest/ihistorical_window_provider.h](../../src/backtest/ihistorical_window_provider.h) — `WindowResult` to be extended
- [src/backtest/historical_window_provider.cpp](../../src/backtest/historical_window_provider.cpp) — cache-only provider (unchanged)
- [src/backtest/backtest_gate.h](../../src/backtest/backtest_gate.h)
- [src/backtest/backtest_gate_controller.cpp](../../src/backtest/backtest_gate_controller.cpp) — `logStructured` lives here
- [src/scanner/kline_cache.h](../../src/scanner/kline_cache.h) — `merge` write-back target
- [src/rest/rest_client.h](../../src/rest/rest_client.h) — `klines(symbol, interval, limit, startTime?, endTime?)` already exists
- [src/rest/rest_client.cpp:611](../../src/rest/rest_client.cpp) — implementation we will call
- [src/rest/rate_limiter.cpp:18](../../src/rest/rate_limiter.cpp) — `klineWeight` confirms `limit > 1000` is a valid tier (weight 10)
- [src/engine/signal_engine.cpp:434](../../src/engine/signal_engine.cpp) — `evaluateBacktestNonBlocking` dispatches `evaluate` onto `boost::asio::thread_pool`
- [src/binance_api.cpp:14](../../src/binance_api.cpp) — legacy wrapper (NOT used by gate path)
- [src/main.cpp:870](../../src/main.cpp) — `BinanceContext ctx; RestClient rest = ctx.makeRestClient();`
- [src/main.cpp:1244-1268](../../src/main.cpp) — startup validation branches
- [docs/design/2026-05-23-backtest-parameter-optimizer-v1.1.md](2026-05-23-backtest-parameter-optimizer-v1.1.md) §6.1
