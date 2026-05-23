# Backtest Parameter Optimizer Gate - v1.1

**Version:** 1.1
**Date:** 2026-05-23
**Status:** DESIGN - pending implementation
**Scope:** New async gate for whitelisted C++ indicator strategies only. The gate sits after signal acceptance and pre-trade preflight, before order placement. Qlib optimization is deferred to v1.2 because the current qlib path only exposes latest SQLite decisions/predictions, not a historical in-memory evaluation contract.

---

## 1. Summary

`BacktestGate` is an optional, fail-closed confirmation and one-shot re-parameterization layer for live strategy signals.

When enabled:

1. A whitelisted indicator strategy emits `Signal::Long` or `Signal::Short` at closed bar `T`.
2. `SignalEngine` runs existing cheap filters and a no-order preflight: direction, confidence, ATR, duplicate-position, account snapshot, sizing, order-cap, exposure, risk gate, and GeminiFilter if enabled.
3. `BacktestGate` loads a historical closed-candle window large enough for the requested optimization. It never assumes the current `KlineCache` buffer has enough bars.
4. Gemini proposes parameter ranges from strategy metadata and a context summary that excludes OOS validation slices and signal bar `T`.
5. The gate expands the ranges into a bounded grid, runs an in-memory backtest with walk-forward IS/OOS folds, filters combos by min-trades and OOS Sortino quality, finds a robust plateau, and finally votes on bar `T`.
6. If the plateau vote passes, `SignalEngine` opens the position with plateau-center params after revalidating final live guards.
7. Any gate failure drops the signal when `shadow_only=false`; in shadow mode, the original signal continues but the gate decision is logged and measured.

When disabled, the gate is bypassed completely. Disabled mode is not a `DropReason` and must preserve current `SignalEngine` behavior bit-for-bit.

---

## 2. Goals & Non-Goals

### Goals

- Reduce trades taken from isolated or overfit parameter peaks.
- Validate that the original signal is robust to nearby parameter perturbations before committing capital.
- Adapt search ranges dynamically by symbol and regime while keeping Gemini away from held-out validation data.
- Keep production behavior observable: every evaluated signal records pass/drop/shadow details, reason codes, durations, and combo counts.
- Preserve existing behavior exactly when `backtest_gate.enabled=false`.

### Non-Goals

- No persistence of optimized params across signals.
- No fallback to original params on enforced gate failure.
- No qlib optimization in v1.1.
- No qlib model retraining.
- No plugin ABI change in v1.1.
- No new IPC service. The Gemini range proposer reuses the existing Python subprocess pattern.
- No general-purpose research backtester. This is a bounded live-gating kernel.

---

## 3. Required Code Changes

| File | Change |
|---|---|
| `src/backtest/backtest_gate.h/.cpp` | New `IBacktestGatePort`, `BacktestGateConfig`, `BacktestGateController`, request/result types. No disabled `DropReason`. |
| `src/backtest/historical_window_provider.h/.cpp` | New bounded historical window provider. Reads closed klines from cache when sufficient; otherwise uses configured history source or returns `InsufficientData`. |
| `src/backtest/parameter_space.h/.cpp` | New `ParamRange`, `ParamPoint`, `ParameterSpace::grid()`, `clampToBudget()`. |
| `src/backtest/walk_forward.h/.cpp` | New `PartitionBuilder::build()` for prompt/calibration/signal-bar partitions and `WalkForwardSplitter::split()` for IS/OOS folds inside calibration only. |
| `src/backtest/backtest_engine.h/.cpp` | New in-memory execution model aligned with live sizing/SL/TP semantics where applicable. |
| `src/backtest/plateau_finder.h/.cpp` | New neighborhood selection on scored grid. |
| `src/backtest/optimizable_strategy.h` | New `IOptimizableStrategy` interface and indicator adapters for whitelisted strategy types. |
| `src/backtest/gemini_range_proposer.h/.cpp` | New prompt builder, Python subprocess invocation, response parser, strict validator. |
| `tools/backtest_range_proposer/main.py` | New Python module `tools.backtest_range_proposer.main`; stdin/input-file JSON to stdout/output-file JSON ranges. |
| `src/engine/signal_engine.h/.cpp` | Add nullable gate port, config, evaluation pool, per-scan budget state, preflight helper, and gate hook. |
| `src/main.cpp` | Parse `backtest_gate`; build data provider and controller only when enabled; wire nullable port into `SignalEngine`. |
| `config.json` | Add disabled-by-default `backtest_gate` block. |
| `tests/test_*` | Add unit and integration tests listed in Section 11. |

Whitelisted v1.1 strategy types:

- `golden_crossover`
- `donchian_5_20_crossover`
- `gartley_day_crossover`

The indicator adapters reimplement only the parameterized signal formulas needed for backtesting. They do not require plugin ABI changes. Tests must compare adapter outputs against the loaded plugin strategies on deterministic klines.

---

## 4. Architecture

```text
SignalEngine processItem
  -> strategy.evaluate(...)
  -> direction/confidence/ATR/tracked-position filters
  -> preflightOpenPosition(..., placeOrders=false)
       account snapshot, sizing, order cap, exposure, risk, GeminiFilter
  -> BacktestGate, if enabled and per-scan budget allows
       HistoricalWindowProvider
       GeminiRangeProposer
       ParameterSpace grid + budget clamp
       WalkForwardSplitter
       BacktestEngine
       PlateauFinder
       Bar-T majority vote
  -> openPositionFromPreflight(...)
       revalidate live guards that can change
       place market + protection orders
```

The gate runs on a dedicated `boost::asio::thread_pool` owned by `SignalEngine` or a small gate wrapper, mirroring the existing Gemini non-blocking pattern. V1.1 uses `worker_pool_size=1` because the current scan loop dispatches at most one gate evaluation at a time; future parallel-symbol or multi-cycle execution can raise this. `SignalEngine` still processes the scan loop sequentially in v1.1, so `backtest_gate.max_evaluations_per_scan_cycle` is mandatory to prevent one scan from spending unbounded time on optimizations.

---

## 5. Public Interfaces

### 5.1 `IBacktestGatePort`

```cpp
namespace backtest {

struct BacktestGateRequest {
    std::string symbol;
    std::string strategyId;
    const strategy::StrategyConfig* baseConfig{nullptr};
    const IOptimizableStrategy* optimizableAdapter{nullptr};
    std::string interval;
    std::vector<Kline> closedKlines;  // includes signal bar T as the last element
    std::chrono::system_clock::time_point signalBarOpenTime;
    strategy::Signal::Direction originalDirection{strategy::Signal::Direction::None};
    double originalAtr{0.0};
    double currentPrice{0.0};
};

enum class DropReason {
    NotEligible,
    InsufficientData,
    GeminiUnavailable,
    GeminiTimeout,
    GeminiInvalidResponse,
    ComboBudgetExhausted,
    NoComboPassedFilter,
    NoPlateauFound,
    MajorityVoteFailed,
    DeadlineExceeded,
    InternalError,
};

struct PassResult {
    strategy::Signal::Direction direction;
    double atr{0.0};
    double initialStopPrice{0.0};
    double slMultiplier{0.0};
    double tpMultiplier{0.0};
    double riskPct{0.0};
    std::unordered_map<std::string, double> optimizedParams;
    double centerSortinoIS{0.0};
    double centerSortinoOOS{0.0};
    int plateauVotePass{0};
    int plateauVoteTotal{0};
    int combosEvaluated{0};
    std::chrono::milliseconds wallTime{};
};

struct DropDetail {
    DropReason reason;
    std::string message;
    int combosEvaluated{0};
    std::chrono::milliseconds wallTime{};
};

using BacktestGateResult = std::variant<PassResult, DropDetail>;

class IBacktestGatePort {
public:
    virtual ~IBacktestGatePort() = default;
    virtual BacktestGateResult evaluate(const BacktestGateRequest& req) const = 0;
};

}  // namespace backtest
```

Disabled mode is handled by `SignalEngine` before the port is called:

```cpp
if (!m_backtestGateConfig.enabled || m_backtestGate == nullptr) {
    co_return GateEvaluation::Bypassed;
}
```

This is required for the acceptance criterion that `enabled=false` keeps current behavior unchanged.

### 5.2 `IOptimizableStrategy`

```cpp
namespace backtest {

struct ParamRange {
    std::string name;
    double min{0.0};
    double max{0.0};
    double step{0.0};
    bool isInteger{false};
};

struct ParamConstraint {
    enum class Kind { LessThan, LessEqual };
    std::string left;
    Kind kind{Kind::LessThan};
    std::string right;
};

struct StrategyParamSpec {
    std::vector<std::string> tunableParams;
    std::vector<ParamRange> defaults;
    std::vector<ParamConstraint> constraints;
    std::unordered_map<std::string, double> currentValues;
};

class IOptimizableStrategy {
public:
    virtual ~IOptimizableStrategy() = default;
    virtual StrategyParamSpec spec(const strategy::StrategyConfig& base) const = 0;

    // klines are closed bars only, ending at the evaluation bar.
    virtual strategy::Signal evaluateWith(
        std::string_view symbol,
        std::string_view interval,
        const std::vector<Kline>& klines,
        const std::unordered_map<std::string, double>& params) const = 0;
};

}  // namespace backtest
```

V1.1 tunables:

| Strategy | Tunables | Constraints |
|---|---|---|
| `golden_crossover` | `ma_short`, `ma_long`, `atr_period`, `sl_multiplier`, `tp_multiplier` | `ma_short < ma_long` |
| `donchian_5_20_crossover` | `short_period`, `long_period`, `atr_period`, `sl_multiplier`, `tp_multiplier` | `short_period < long_period` |
| `gartley_day_crossover` | `fast_period`, `slow_period`, `offset`, `conf_threshold`, `atr_period`, `sl_multiplier`, `tp_multiplier` | `fast_period <= slow_period` |

`risk_pct` is intentionally not a v1.1 tunable. It remains a portfolio-level decision owned by live risk and exposure controls; per-signal backtests should optimize signal shape and exit geometry, not scale. Tuning `risk_pct` from recent backtests would tend to push risk higher exactly when a regime looks clean and then over-leverage after regime shifts.

`SignalEngine` must not call the enforced gate for qlib strategies in v1.1. If a qlib strategy reaches `BacktestGateController` directly, the controller returns `DropReason::NotEligible`; shadow metrics may record this, but qlib order flow is not vetoed by the gate in v1.1.

---

## 6. Algorithm Detail

### 6.1 Entry, Preflight, And Data Sizing

The gate runs only after:

- `signal.direction != None`
- `signal.confidence >= cfg.minConfidence`
- ATR is finite and positive
- no tracked position exists for the symbol
- preflight sizing/order-cap/exposure/risk/GeminiFilter allows the trade
- per-scan backtest budget is not exhausted

Historical data sizing:

```cpp
const auto spec = adapter->spec(strategyConfig);
const int slowestParamMax = computeSlowestMax(spec);
const int requiredClosedBars = std::max(
    config.data.windowMinCandles,
    config.data.windowSlowestMultiplier * slowestParamMax);

auto closedKlines = historicalWindowProvider.closedWindow(
    symbol,
    interval,
    requiredClosedBars,
    signalBarOpenTime);

if (closedKlines.size() < requiredClosedBars) {
    return DropDetail{DropReason::InsufficientData, ...};
}
```

`HistoricalWindowProvider` contract:

- It returns closed bars only.
- The last returned bar must be the signal bar `T`.
- It must report both `required_bars` and `available_bars` on insufficiency.
- It may use `KlineCache` only if the cache contains enough closed bars. The current default cache size of 240 is not enough for the default 2000-bar gate window.
- If `history_source="cache_only"`, enabling the gate requires scanner/cache capacity to be configured high enough. Otherwise invalid config disables the gate at startup.
- If `history_source="cache_then_rest"`, runtime REST fetches are bounded by request and wall-clock limits and count against the gate deadline.

### 6.2 Held-Out Partitions

For a signal at closed bar `T`, the returned window is partitioned as:

```text
[prompt_context][calibration_window][signal_bar_T]
```

Rules:

- Gemini prompt data is built strictly from `prompt_context` and ends before `calibration_window`.
- `calibration_window` is passed to `WalkForwardSplitter`; OOS validation slices are the OOS portions of folds inside this window, not a separate top-level partition.
- Gemini never receives raw calibration candles that later become OOS, OOS aggregate performance, or the signal bar `T`.
- Walk-forward optimization excludes `prompt_context` and signal bar `T`.
- Plateau selection may use OOS scores from the walk-forward folds inside `calibration_window`.
- The final majority vote uses `T` only after plateau selection is complete.

This makes the claim precise: Gemini does not see calibration OOS or `T`; the optimizer may score OOS by design inside the calibration window; `T` is a final confirmation check, not part of range proposal or plateau selection.

### 6.3 Gemini Range Proposal

`GeminiRangeProposer` builds a bounded JSON payload:

```json
{
  "symbol": "BTCUSDT",
  "interval": "1h",
  "strategy_id": "golden_crossover",
  "tunable_params": ["ma_short", "ma_long", "atr_period", "sl_multiplier", "tp_multiplier"],
  "current_values": {"ma_short": 50, "ma_long": 200, "atr_period": 14, "sl_multiplier": 1.5, "tp_multiplier": 3.0},
  "constraints": [{"left": "ma_short", "op": "<", "right": "ma_long"}],
  "prompt_context_summary": {
    "n_bars": 1200,
    "last_included_bar_open_time": 1747105200,
    "computed_from_prompt_context_only": true,
    "excludes_calibration_and_signal_bar": true,
    "ret_30d_pct": -0.084,
    "ret_90d_pct": 0.142,
    "atr_pct_current": 0.022,
    "atr_pct_p90": 0.041,
    "regime_hint": "uptrend_then_pullback"
  },
  "signal": {"direction": "Long", "bar_open_time": 1748905200},
  "budget": {"max_total_combos": 6000}
}
```

All aggregate stats in `prompt_context_summary` are computed strictly within the prompt context window. They must not read `calibration_window`, any WF OOS fold, or signal bar `T`.

The C++ side writes the payload under `tmp/backtest_range_proposer/eval-<id>/input.json`, invokes:

```text
python -m tools.backtest_range_proposer.main <input-json>
```

and parses stdout JSON. The module path is exactly `tools.backtest_range_proposer.main`, backed by `tools/backtest_range_proposer/main.py`.

Validation:

- Every range name must be in `spec.tunableParams`.
- `min <= max`, `step > 0`, all values finite.
- Integer ranges snap to integer grid points.
- Constraints are checked against every grid point.
- Grid size after constraints must be positive.
- If the grid exceeds `budget.max_total_combos`, `clampToBudget()` widens steps before evaluation. If it still exceeds budget, drop `ComboBudgetExhausted`.

### 6.4 Walk-Forward Split

```cpp
auto partitions = PartitionBuilder::build(closedKlines, config);
auto folds = WalkForwardSplitter::split(
    partitions.calibrationWindow,
    config.walkForward.folds,
    config.walkForward.isFraction);
```

Requirements:

- `PartitionBuilder::build()` returns `promptContext`, `calibrationWindow`, and `signalBar`; it must not expose `validation_oos_slices` as a top-level segment.
- IS/OOS folds are created only from `partitions.calibrationWindow`.
- OOS slices do not overlap.
- Signal bar `T` is not in any IS or OOS slice.
- `promptContext` is not in any IS or OOS slice.
- Each fold must have enough bars for the slowest period plus one next-open entry bar.
- Unit tests must assert exact index boundaries.

### 6.5 Backtest Execution Model

The backtest engine should mirror live execution closely enough that optimized params mean the same thing in production:

- apply `cfg.minConfidence`
- use strategy-provided ATR when present, otherwise optimized `atr_period`
- open at next bar open after a closed-bar signal
- model SL/TP from `sl_multiplier` and `tp_multiplier`
- support fixed take-profit percent and leverage if live config uses it
- model `maxHoldDuration`
- deduct configured taker fees and optional slippage
- apply tick/step/min-notional rounding when symbol metadata is available
- never model order-cap, exposure, risk controller, or GeminiFilter per combo; those are preflight/live gates, not per-combo historical strategy rules

If both SL and TP are hit inside the same candle, v1.1 uses conservative ordering:

- long: stop first when `low <= stop` and `high >= tp`
- short: stop first when `high >= stop` and `low <= tp`

The engine returns per-fold IS and OOS stats:

```cpp
struct BacktestStats {
    int numTrades{0};
    double sortino{0.0};
    double sharpe{0.0};
    double profitFactor{0.0};
    double maxDrawdown{0.0};
    double winRate{0.0};
};
```

Sortino:

```text
sortino = mean(pnlPct) / downside_std(pnlPct)
```

Annualization is reported separately. Filter decisions use non-annualized Sortino to avoid timeframe/sample-size blowups.

Determinism: v1.1 backtests use no RNG. With identical closed klines, config, symbol metadata, and default `slippage_bps=0.0`, the engine must produce identical trades and stats. If random slippage is added later, the slippage model must expose an explicit seed config and include that seed/model version in cache keys.

### 6.6 Filter Chain

For each combo:

1. Every fold must have `numTrades >= filters.min_trades_per_fold`.
2. `mean(sortino_oos)` and `mean(sortino_is)` must be finite and positive.
3. `mean(sortino_oos) >= max(filters.oos_is_ratio_threshold * mean(sortino_is), filters.min_oos_sortino)`.

The absolute OOS floor prevents weak ratios from passing, for example IS Sortino `0.10` and OOS Sortino `0.06`.

Zero surviving combos returns `DropReason::NoComboPassedFilter`.

### 6.7 Plateau Finder

- Pick `best` by highest mean OOS Sortino among surviving combos.
- Build a neighborhood around `best` using `plateau.neighborhood_radius`.
- For grids above `plateau.max_neighborhood_size`, restrict neighborhood dimensions by local sensitivity around `best`.
- A point counts as plateau only if it also survived the filter chain.
- If `plateau_pass_count / neighborhood_size < plateau.min_pass_fraction`, drop `NoPlateauFound`.
- Center params are the arithmetic mean of surviving plateau points, snapped by param type and revalidated against constraints.

### 6.8 Majority Vote On Signal Bar T

Build a distinct vote set:

- Start with every surviving plateau point.
- Compute the snapped plateau center from Section 6.7.
- Add the center as one synthetic vote only if its snapped params are not equal to an existing plateau point.

For each distinct vote point:

```cpp
auto voteSignal = adapter->evaluateWith(symbol, interval, klinesThroughT, params);
```

Count votes where:

- `voteSignal.direction == originalDirection`
- `voteSignal.confidence >= baseConfig.minConfidence`
- ATR is finite and positive

`vote_total` is the number of distinct vote points. If `vote_pass / vote_total >= vote.threshold_fraction`, pass. Otherwise drop `MajorityVoteFailed`.

Pass output:

- `direction` is the original direction.
- `optimizedParams` are the plateau center params.
- `atr` is recomputed through bar `T` using optimized `atr_period`.
- `initialStopPrice = currentPrice +/- center.sl_multiplier * atr`.
- `riskPct` comes from the base live risk config and preflight snapshot; it is not optimized in v1.1.

### 6.9 Deadline And Cancellation

The whole gate uses one steady-clock deadline. It is checked:

- before/after Gemini
- before grid expansion
- between combos
- between folds
- before vote

Deadline exceed returns `DeadlineExceeded`; partial results are discarded. Cancellation is cooperative: once the deadline or cancellation token is observed, no new combo or fold starts. A mid-backtest cancellation may finish the current atomic step, but combo `i+1` must not start after combo `i` observes cancellation.

### 6.10 Cache

LRU key:

```text
v1.1|symbol|strategy_id|interval|signal_bar_open_time|gate_config_hash|strategy_config_hash|history_window_signature
```

The cache stores pass and drop results. Config/hash inclusion prevents stale drops or passes after config changes. TTL and max entries are configurable.

---

## 7. SignalEngine Integration

### 7.1 Flow

Pseudo-flow for indicator strategies:

```cpp
auto signal = strategy->evaluate(symbol, interval, klines);
if (signal.direction == strategy::Signal::Direction::None) co_return;
if (signal.confidence < cfg.minConfidence) co_return;

auto preflight = co_await preflightOpenPosition(
    symbol, interval, signal.direction, atr, currentPrice, cfg, signal.reason);
if (!preflight.allowed) co_return;

if (!m_backtestGateConfig.enabled || !isBacktestEligible(cfg.type)) {
    co_await openPositionFromPreflight(preflight, originalParams);
    co_return;
}

auto gateRes = co_await evaluateBacktestNonBlocking(buildBacktestRequest(...));
if (auto* drop = std::get_if<backtest::DropDetail>(&gateRes)) {
    recordBacktestDrop(*drop);
    if (!m_backtestGateConfig.shadowOnly) co_return;
    co_await openPositionFromPreflight(preflight, originalParams);
    co_return;
}

auto pass = std::get<backtest::PassResult>(gateRes);
auto optimized = applyOptimizedToConfig(cfg, pass.optimizedParams);
co_await openPositionFromPreflight(preflight, optimized, pass);
```

`openPositionFromPreflight()` must revalidate mutable live state before placing orders:

- tracker reservation
- current account snapshot if the preflight snapshot is stale
- current price/tick/step conversion
- risk/order/exposure gates if configured to recheck

### 7.2 Per-Scan Budget

`SignalEngine` maintains:

```cpp
int m_backtestEvaluationsThisCycle{0};
BacktestCycleGate m_backtestCycleGate;
```

The per-scan budget counts only gate-eligible indicator candidates. It never counts qlib, non-whitelisted strategies, or candidates that fail cheap/preflight guards before the gate.

If `max_evaluations_per_scan_cycle` is exhausted:

- if `close_gate_on_budget_exhausted=true`, the backtest gate is closed only for the remaining gate-eligible indicator candidates in the current scan cycle
- enforced mode drops those remaining eligible indicator signals with reason `BacktestBudgetExhausted`
- shadow mode bypasses veto for those indicator signals but records `BACKTEST_GATE_SKIPPED`
- qlib and other non-eligible paths continue through their current flow and are not blocked by the backtest budget

This prevents a sequential scan cycle from spending up to `deadline_seconds` on every indicator candidate without closing the whole `SignalEngine` cycle. The default budget is provisional; Phase 5 must measure `indicator_candidates_per_cycle` and set the enforce-mode budget at or above the chosen coverage percentile before live enforcement.

### 7.3 Qlib Interaction

Qlib strategies are not optimized in v1.1.

- `processQlibCandidates()` keeps the current arbitration and shadow metrics behavior.
- If `backtest_gate.enabled=true`, qlib candidates are tagged `not_eligible` for gate metrics in shadow-only rollout, but the gate does not veto them.
- Future qlib support requires a historical prediction accessor and a pure policy simulator for `qlib_model_signal` and `qlib_strategy_signal`.

---

## 8. Anti-Overfit Defenses

| Layer | Defense | Concrete Check |
|---|---|---|
| L1 - Prompt isolation | Gemini cannot tune to OOS or signal bar | Prompt summary is computed only from `prompt_context`; calibration WF OOS folds and bar `T` are excluded. |
| L2 - Walk-forward IS/OOS | Reject in-sample-only combos | Rolling folds over calibration window, signal bar excluded. |
| L3 - OOS/IS ratio plus floor | Penalize strong IS / weak OOS and low absolute quality | `mean_oos_sortino >= max(threshold * mean_is_sortino, min_oos_sortino)`. |
| L4 - Min trades | Avoid tiny-sample winners | Every fold needs at least configured trades. |
| L5 - Robust plateau | Reject isolated peaks | Neighborhood pass fraction must meet threshold. |
| L6 - Signal-bar vote | Require current signal robustness | Distinct plateau/center vote points independently emit original direction on `T`. |
| L7 - Combo budget | Bound multiple testing and latency | Grid clamp plus hard combo cap. |
| L8 - Shadow rollout | Validate before enforcement | 2-4 week paper/shadow acceptance period. |

---

## 9. Config Schema

```json
"backtest_gate": {
  "enabled": false,
  "shadow_only": true,
  "worker_pool_size": 1,
  "deadline_seconds": 90,
  "max_evaluations_per_scan_cycle": 3,
  "close_gate_on_budget_exhausted": true,

  "data": {
    "history_source": "cache_only",
    "window_min_candles": 2000,
    "window_slowest_multiplier": 10,
    "runtime_rest_fetch_enabled": false,
    "runtime_rest_fetch_timeout_seconds": 10,
    "max_rest_requests_per_signal": 3
  },

  "walk_forward": {
    "folds": 4,
    "is_fraction": 0.75,
    "prompt_context_fraction": 0.50,
    "signal_bar_holdout": true
  },

  "filters": {
    "min_trades_per_fold": 10,
    "oos_is_ratio_threshold": 0.5,
    "min_oos_sortino": 0.3
  },

  "plateau": {
    "neighborhood_radius": 1,
    "max_neighborhood_size": 81,
    "min_pass_fraction": 0.5
  },

  "vote": {
    "threshold_fraction": 0.6
  },

  "budget": {
    "max_total_combos": 6000
  },

  "gemini": {
    "python_path": "python",
    "module_name": "tools.backtest_range_proposer.main",
    "working_directory": ".",
    "runtime_dir": "tmp/backtest_range_proposer",
    "model": "gemini-3.1-pro-preview",
    "timeout_seconds": 8,
    "retries": 1,
    "stale_runtime_ttl_hours": 24
  },

  "cache": {
    "ttl_seconds": 7200,
    "max_entries": 256
  },

  "fee": {
    "taker_fee_rate": 0.0004,
    "slippage_bps": 0.0
  }
}
```

Validation:

- Invalid config disables `backtest_gate` and logs an error.
- `enabled=false` bypasses all gate logic.
- `history_source=cache_only` requires enough configured cache/history capacity for `window_min_candles`; otherwise startup disables the gate.
- Runtime REST fetching is off by default.
- `worker_pool_size` defaults to `1` in v1.1. Higher values are reserved for future parallel scan execution and should be rejected or warned unless the implementation proves concurrent dispatch.
- `max_evaluations_per_scan_cycle >= 0`; `0` means no gate evaluations and all gate-eligible indicator signals are skipped in enforce mode.
- `close_gate_on_budget_exhausted=true` closes only the backtest gate for remaining gate-eligible indicator candidates in the current scan cycle. It does not close the qlib path or the whole scanner.
- Before enforce rollout, Phase 5 shadow metrics must show the configured budget covers the chosen percentile of gate-eligible indicator candidates per cycle. If p95 is above the configured budget, do not enable enforcement until the budget/deadline tradeoff is adjusted.

---

## 10. Observability

Structured single-line JSON events:

| Event | Fields |
|---|---|
| `BACKTEST_GATE_ENTER` | `symbol, strategy_id, interval, direction, bar_open_time, required_bars, available_bars, combo_budget` |
| `BACKTEST_GATE_SKIPPED` | `symbol, strategy_id, reason, shadow_only` |
| `BACKTEST_GATE_DATA_READY` | `history_source, window_size, first_bar, last_bar` |
| `BACKTEST_GATE_GEMINI_OK` | `ranges_count, ranges_dims, gemini_latency_ms, prompt_cutoff_bar` |
| `BACKTEST_GATE_GRID_BUILT` | `combos_total, combos_after_constraints, combos_after_clamp, folds` |
| `BACKTEST_GATE_FILTER_DONE` | `combos_surviving, min_trades_rejects, oos_is_rejects` |
| `BACKTEST_GATE_PLATEAU` | `best_oos_sortino, plateau_size, plateau_pass_count, center_params` |
| `BACKTEST_GATE_VOTE` | `vote_pass, vote_total, threshold, decision` |
| `BACKTEST_GATE_PASS` | `optimized_params, atr, sl, tp, risk_pct, wall_time_ms` |
| `BACKTEST_GATE_DROP` | `reason, message, wall_time_ms, partial_combos` |

Metrics:

- `backtest_gate_evaluations_total{outcome="pass|drop|skipped|bypassed"}`
- `backtest_gate_drops_total{reason="..."}`
- `backtest_gate_eval_duration_seconds`
- `backtest_gate_gemini_latency_seconds`
- `backtest_gate_combos_per_signal`
- `backtest_gate_plateau_size`
- `backtest_gate_vote_ratio`
- `backtest_gate_cache_hits_total`
- `backtest_gate_cache_misses_total`
- `backtest_gate_history_available_bars`
- `backtest_gate_budget_exhausted_total`
- `backtest_gate_indicator_candidates_per_cycle`
- `backtest_gate_worker_pool_queue_depth`

---

## 11. Testing Strategy

### Unit Tests

- `test_parameter_space.cpp`: grid size, constraint filtering, integer snapping, budget clamp invariants.
- `test_partition_builder.cpp`: exact top-level boundaries for `prompt_context`, `calibration_window`, and `signal_bar_T`; no top-level `validation_oos_slices`.
- `test_walk_forward.cpp`: exact fold boundaries inside `calibration_window`; prompt context and signal bar are excluded; OOS slices do not overlap.
- `test_historical_window_provider.cpp`: cache-only sufficient, cache-only insufficient, cache-then-rest bounded fetch, closed-bar-only output.
- `test_backtest_engine.cpp`: deterministic trades with no RNG, long/short symmetry, confidence filter, SL/TP same-candle conservative ordering, fee/slippage deduction, fixed take-profit percent parity.
- `test_plateau_finder.cpp`: isolated peak rejected, broad plateau accepted, high-dimensional neighborhood cap.
- `test_gemini_range_proposer.cpp`: prompt excludes calibration/OOS/T, prompt aggregate stats are computed only from the prompt slice, module path is `tools.backtest_range_proposer.main`, parser rejects malformed/out-of-spec ranges.
- `test_indicator_adapters.cpp`: adapter outputs match plugin outputs for whitelisted strategies on deterministic klines.

### Integration Tests

- `test_backtest_gate.cpp`:
  - happy path returns `PassResult`
  - qlib strategy returns `NotEligible`
  - insufficient historical bars returns `InsufficientData`
  - Gemini timeout/unavailable/invalid response return correct reasons
  - no surviving combos returns `NoComboPassedFilter`
  - plateau vote failure returns `MajorityVoteFailed`
  - cache key changes with config hash and history signature
  - deadline returns `DeadlineExceeded`
  - deadline cancellation is cooperative; after combo `i` observes cancellation, combo `i+1` is not started
  - concurrent evaluations for the same key share or serialize cache work without data races or lock starvation

- `test_signal_engine.cpp`:
  - `enabled=false` never calls the gate and preserves current open/drop behavior
  - enforce pass opens with optimized params
  - enforce drop skips order placement
  - shadow drop opens with original params and records gate decision
  - per-scan budget exhaustion closes only remaining gate-eligible indicator candidates, not qlib or non-eligible paths
  - preflight failure does not call the gate
  - final live revalidation can still prevent order placement after gate pass

---

## 12. Decision Log

| # | Decision | Rationale |
|---|---|---|
| 1 | Disabled gate is a bypass, not a drop. | Required to preserve current behavior with `enabled=false`. |
| 2 | V1.1 supports indicator strategies only. | Current qlib path lacks historical evaluation and policy simulation contracts. |
| 3 | Historical data is a first-class provider. | Current `KlineCache` default is too small for 2000+ bars. |
| 4 | Prompt/calibration/T partitions are explicit, with WF OOS inside calibration. | Prevents overstating held-out guarantees and avoids Gemini seeing validation data. |
| 5 | Gate runs after preflight, before order placement. | Avoids expensive optimization for trades that existing guards already reject. |
| 6 | Final live guards revalidate after a gate pass. | Account, tracker, and price state can change during a 90s evaluation. |
| 7 | Backtest uses non-annualized Sortino for filters. | Avoids small-sample annualization blowups. |
| 8 | Per-scan gate budget defaults to 3 but must be calibrated before enforcement. | Existing filter rollout patterns already use a small per-cycle budget; shadow metrics must verify coverage before blocking live trades. |
| 9 | Plugin ABI remains unchanged in v1.1. | Indicator adapters can be implemented and tested in core without widening plugin contracts. |
| 10 | `risk_pct` is fixed in v1.1, not optimized. | Risk sizing is portfolio-level control; tuning it per signal optimizes scale and can over-leverage regime-specific winners. |
| 11 | Worker pool defaults to 1 in v1.1. | The current scan path dispatches gate evaluations sequentially, so extra workers would be unused capacity until parallel scan execution exists. |

---

## 13. Risks & Mitigations

| Risk | Severity | Mitigation |
|---|:---:|---|
| Insufficient historical data | High | HistoricalWindowProvider, startup config validation, explicit `available_bars` logs. |
| Gemini meta-overfit | High | Prompt is computed only from `prompt_context`; calibration OOS and `T` remain separate. |
| Backtest/live mismatch | High | BacktestExecutionModel parity tests against `SignalEngine` semantics. |
| Sequential scan stalls | Medium | Per-scan budget, deadline, skip metrics. |
| Qlib accidentally blocked | Medium | Qlib is not gate-enforced in v1.1; shadow metrics only. |
| Stale cache after config/data changes | Medium | Cache key includes config hash, strategy hash, and history signature. |
| REST fetch latency | Medium | Runtime REST fetch disabled by default; bounded when enabled. |
| Adapter drift from plugin logic | Medium | Golden adapter-vs-plugin tests for each whitelisted strategy, plus a `docs/sdk/plugin-review-checklist.md` item requiring adapter/test updates whenever plugin formulas or tunables change. |

---

## 14. Implementation Phases

Each phase is independently mergeable behind `backtest_gate.enabled=false`.

### Phase 0 - Integration Contracts

- Add disabled-bypass semantics.
- Add preflight/open split design in `SignalEngine`.
- Add per-scan backtest budget state.
- Add historical data provider interface.

### Phase 1 - Numerical Kernel

- `parameter_space`
- `walk_forward`
- `historical_window_provider`
- `backtest_engine`
- unit tests

### Phase 2 - Indicator Optimization

- `plateau_finder`
- `optimizable_strategy`
- whitelisted indicator adapters
- adapter-vs-plugin tests
- add or update `docs/sdk/plugin-review-checklist.md` for adapter/formula drift
- `BacktestGateController` with fake range proposer

### Phase 3 - Gemini Wiring

- `tools/backtest_range_proposer/main.py`
- `gemini_range_proposer`
- strict response validation
- cache with config/data signatures
- structured logs and metrics

### Phase 4 - SignalEngine Integration

- preflight/open split
- `evaluateBacktestNonBlocking()`
- enforce/shadow behavior
- budget exhaustion behavior
- integration tests

### Phase 5 - Shadow Rollout

- Enable `backtest_gate.enabled=true, shadow_only=true`.
- Run at least 2 weeks.
- Record `indicator_candidates_per_cycle` distribution and set enforce-mode `max_evaluations_per_scan_cycle` at or above the chosen coverage percentile, with p95 as the default target.
- Require p95 evaluation duration <= deadline, zero `InternalError` drops, no scan liveness regression, and no qlib/non-eligible budget blocking.

### Future v1.2 - Qlib Support

- Historical prediction accessor.
- Pure qlib policy simulator for model-signal and strategy-signal paths.
- TopK/threshold/hold-period sweep without retraining.
- Long-only/short-capability contract documented explicitly.

---

## 15. Assumptions

| # | Assumption |
|---|---|
| A1 | Latency budget remains 90s per evaluated signal. |
| A2 | Default per-scan evaluations is 3 for shadow rollout and must be recalibrated from candidate-per-cycle metrics before enforcement. |
| A3 | Default historical window is at least 2000 closed bars. |
| A4 | V1.1 excludes qlib from gate enforcement. |
| A5 | Existing indicator plugin ABI is unchanged. |
| A6 | Final order placement revalidates mutable live guards after a gate pass. |
| A7 | Shadow mode must never veto a trade. |
| A8 | Disabled mode must not call the gate. |
| A9 | `risk_pct` is fixed by live risk config, not optimized. |
| A10 | Worker pool size remains 1 until scan execution actually dispatches parallel gate jobs. |

---

## 16. Open Questions

1. Live cutover KPI: proposed minimum is 2 weeks shadow, p95 duration within deadline, no `InternalError`, and pass-set hypothetical Sharpe better than original by at least 0.2.
2. Whether runtime REST fetch should ever be enabled for live enforcement, or only for shadow/research.
3. Exact allowed ranges per whitelisted indicator strategy before Gemini clamps.
4. Exact enforce-mode budget after Phase 5 measures p95 gate-eligible indicator candidates per cycle.
5. Qlib v1.2 policy simulator scope and whether short signals are supported.

---

## 17. Glossary

| Term | Meaning |
|---|---|
| IS | In-sample fold segment. |
| OOS | Out-of-sample fold segment used for validation scoring. |
| Bar T | The closed candle that produced the original live signal. |
| Prompt context | Data summary visible to Gemini; excludes OOS and bar `T`. |
| Calibration window | Window used for walk-forward scoring; excludes bar `T`. |
| Plateau | Neighborhood of surviving combos around the best OOS-scored combo. |
| Vote | Final bar-`T` check over plateau points after selection. |

---

## 18. Acceptance Criteria

- All unit and integration tests pass.
- `enabled=false` produces no gate calls and preserves existing `SignalEngine` behavior.
- `shadow_only=true` never vetoes a trade.
- Qlib signals are not gate-enforced in v1.1.
- Historical data insufficiency is observable with required/available bar counts.
- Gemini prompt tests prove OOS candles and bar `T` are excluded.
- Backtest/live parity tests cover confidence, ATR, SL/TP, fees, tick/step rounding, and fixed take-profit percent.
- A reviewer can trace every enforced drop to one layer in Section 8 using logs alone.
