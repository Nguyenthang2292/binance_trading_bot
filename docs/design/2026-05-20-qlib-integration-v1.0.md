# Microsoft Qlib Integration Plan - v1.1

**Date:** 2026-05-20
**Revised:** 2026-05-20 (v1.1 — post-review updates)
**Status:** PROPOSED
**Audience:** AI agents, human developers

**Related project areas:**
- `src/strategy/istrategy.h` - live strategy ABI and signal contract
- `src/engine/signal_engine.cpp` - live signal evaluation and order path
- `src/scanner/kline_cache.h` - runtime kline cache used by strategies and filters
- `src/engine/gemini_filter.h` - existing C++ to Python sidecar integration pattern
- `docs/sdk/writing-a-strategy-plugin.md` - strategy plugin ABI guide

**External source reviewed:**
- Microsoft Qlib repository: https://github.com/microsoft/qlib
- Reviewed commit: `d5379c520f66a39953bad76234a7019a72796fd0`
- Commit date: 2026-04-22

---

## 1. Executive Summary

Qlib should not be embedded directly into the C++ live trading runtime.

The recommended integration is:

1. Run Qlib as an offline or sidecar Python research pipeline.
2. Use Qlib for data preparation, feature engineering, model training, prediction generation, and offline backtesting.
3. Export model scores into a SQLite database (primary store from MVP).
4. Add a C++ strategy plugin that reads those scores and returns the existing `strategy::Signal`.
5. Keep the current C++ bot responsible for live execution: scanner, ATR fallback, risk controls, exposure controls, sizing, order placement, stop loss, take profit, trailing stop, user data reconciliation, and loss management.

This preserves the strongest part of both systems:

| System | Should Own |
|---|---|
| Qlib | Research, ML training, feature engineering, prediction scores, offline backtest |
| Current C++ bot | Live Binance connectivity, futures execution, risk gates, order lifecycle, position tracking |

---

## 2. Current Project Fit

The current bot is already organized around a narrow strategy contract:

```cpp
virtual Signal evaluate(
    std::string_view symbol,
    std::string_view interval,
    const std::vector<Kline>& klines) const = 0;
```

`SignalEngine` calls this strategy contract for each `(symbol, interval, strategy)` work item, then applies:

1. confidence filtering
2. ATR fallback
3. current price validation
4. tracked position de-duplication
5. risk analytics gate
6. order cap and exposure controls
7. Gemini filter if enabled
8. order placement
9. TP, SL, trailing stop, and time exit handling

This means Qlib does not need to know how to place Binance Futures orders. It only needs to provide a model score that can be mapped into `Long`, `Short`, or `None`.

The project already has a safe precedent for Python sidecars: `GeminiFilterController` invokes a Python module with JSON input, timeout handling, cache keys, runtime directories, and fail-closed behavior. Qlib integration should follow this sidecar style instead of linking Python or Qlib into the C++ binary. Python environment isolation (venv path, interpreter path) should follow the same pattern already established for the Gemini sidecar.

**Signal arbitration note:** Each strategy plugin is evaluated independently by `SignalEngine` and subject to tracked position de-duplication (step 4). If both a Qlib plugin and a rule-based plugin signal opposite directions on the same symbol simultaneously, the first signal to produce an open position wins and the second is blocked by de-duplication. There is no cross-strategy aggregation. This is the intended behavior for the initial `standalone strategy` usage.

---

## 3. Qlib Capabilities Relevant To This Bot

Qlib provides:

1. Data layer and expression engine for financial features.
2. CSV and Parquet to Qlib `.bin` conversion through `scripts/dump_bin.py`.
3. Model training workflows such as `qrun` and workflow-by-code.
4. Forecast models such as LightGBM, linear models, PyTorch models, and research models.
5. Portfolio strategy and backtesting modules.
6. Online serving components for model update and prediction workflows.

Important limitation for this project:

Qlib's included crypto collector uses Coingecko data and its README states that the crypto dataset only supports data retrieval, not backtesting, because it lacks OHLC data. Therefore, this bot should not rely on Qlib's default crypto collector for Binance Futures strategy research. It should export high-quality Binance OHLCV data from the bot's own data source.

---

## 4. Recommended Architecture

```mermaid
flowchart LR
    A["Binance historical klines"] --> B["tools/qlib_bridge/export_binance_klines.py"]
    B --> C["CSV or Parquet OHLCV dataset"]
    C --> D["Qlib dump_bin.py"]
    D --> E["Qlib .bin provider_uri"]
    E --> F["Qlib training and backtest workflow"]
    F --> G["Model artifact and prediction scores"]
    G --> H["data/qlib_predictions.db (SQLite)"]
    H --> I["C++ qlib_model_signal plugin"]
    I --> J["strategy::Signal"]
    J --> K["SignalEngine live risk and order path"]
```

Primary boundary:

- Qlib ends at `prediction score`.
- The C++ bot starts at `strategy::Signal`.

This boundary is intentionally simple, testable, and robust.

---

## 5. Data Integration Plan

### 5.1 Export Binance Data

Create a new bridge package:

```text
tools/
  qlib_bridge/
    export_binance_klines.py
    train_workflow.py
    predict_latest.py
    requirements.txt
    README.md
```

`requirements.txt` must pin all versions (qlib, lightgbm, pandas, numpy, pyarrow). The Python interpreter used should match the one configured for the Gemini sidecar — see `src/engine/gemini_filter.h` for the existing venv/path resolution pattern.

Exporter output schema:

```text
datetime,symbol,open,high,low,close,volume,factor,quote_volume,trade_count
```

Rules:

- `factor = 1.0` for crypto.
- Use UTC timestamps unless a later design explicitly introduces timezone conversion.
- Use Binance symbols as stored in the bot, for example `BTCUSDT`.
- Preserve raw OHLCV values. Do not use Coingecko adjusted crypto fields.
- Export one file per symbol or one file with a `symbol` column. Qlib supports both, but a single Parquet or CSV with `symbol` is easier for batch operations.

### 5.2 Frequency Mapping

Current bot intervals:

| Bot interval | Qlib frequency |
|---|---|
| `1d` | `day` |
| `4h` | `240min` |
| `1h` | `60min` |
| `30m` | `30min` |

Qlib frequency parsing supports `day` and minute-style frequencies such as `30min`, `60min`, and `240min`.

**24/7 crypto calendar requirement:** Qlib's default calendar is built for stock-market sessions and may skip rows at midnight boundaries or weekends. A custom calendar provider must be registered before calling `qlib.data.D.features()`. Validation smoke test must confirm no rows are dropped at `00:00 UTC` crossings and across any Sunday-to-Monday boundary.

### 5.3 Convert To Qlib Format

Use Qlib's `dump_bin.py` rather than inventing a local binary format.

Example command shape:

```bash
python <qlib_repo>/scripts/dump_bin.py dump_all \
  --data_path data/qlib_csv/binance_30min \
  --qlib_dir data/qlib/binance_30min \
  --freq 30min \
  --date_field_name datetime \
  --symbol_field_name symbol \
  --include_fields open,high,low,close,volume,factor,quote_volume,trade_count \
  --file_suffix .parquet
```

The bridge should support both:

1. Full rebuild for research.
2. Incremental update for online prediction.

---

## 6. Model And Research Workflow

### 6.1 MVP Model

Start with LightGBM because Qlib already includes a standard LightGBM workflow and it is much faster to validate than a deep model.

Initial target:

```text
future_return_N = Ref($close, -N) / $close - 1
```

Candidate horizons:

| Interval | Horizon |
|---|---|
| `30min` | 4 to 16 bars |
| `1h` | 4 to 12 bars |
| `4h` | 3 to 10 bars |
| `day` | 1 to 5 bars |

Start with one interval, preferably `1h`, then expand.

### 6.2 Feature Set

Start with a simple OHLCV feature set before Alpha158/Alpha360 customization:

- returns over multiple windows
- moving average distance
- rolling volatility
- rolling high and low breakout distance
- volume z-score
- ATR-style range features
- cross-timeframe context if data pipeline is stable

Only after this baseline works should the project add a custom Qlib handler or adapt Alpha158-style features.

### 6.3 Research Metrics

Track at least:

- IC
- Rank IC
- long-short return
- turnover
- score stability by interval
- score decay by prediction age
- live hit rate after exported predictions are consumed by the bot

Do not treat Qlib backtest PnL as final live PnL because the live bot has separate futures-specific mechanics: leverage, stop loss, take profit, trailing stop, DCA/loss manager, exposure control, and order caps.

### 6.4 Walk-Forward Validation Requirements

Walk-forward validation is mandatory before any prediction is consumed live. The following spec must be followed to prevent time-series leakage:

| Parameter | Requirement |
|---|---|
| Train window | Rolling 6 months minimum |
| Test window | 1 month out-of-sample |
| Embargo period | `horizon_bars` bars between train end and test start (prevents label overlap) |
| Purging | Remove all rows whose label window overlaps with the test period from the train set |
| Refit cadence | Monthly re-train with walk-forward step |

The embargo period is the most critical control. For a 4-bar horizon on `1h` data, 4 hours of rows must be dropped between the last train row and the first test row. Skipping this step will produce inflated IC that collapses in live trading.

---

## 7. Prediction Store Contract

**Decision: use SQLite from MVP.** JSON is retained here as a reference but is not the recommended starting point. SQLite is chosen because:
- Atomic writes are guaranteed on Windows (no rename race condition)
- Stale queries are simple SQL predicates
- Test fixtures are portable `.db` files
- No partial-read risk for the C++ plugin

### 7.1 SQLite Schema

Path:

```text
data/qlib_predictions.db
```

Table:

```sql
CREATE TABLE qlib_predictions (
    model_id TEXT NOT NULL,
    symbol TEXT NOT NULL,
    interval TEXT NOT NULL,
    asof_open_time_ms INTEGER NOT NULL,
    generated_at_ms INTEGER NOT NULL,
    score REAL NOT NULL,
    rank INTEGER,
    score_percentile REAL,
    PRIMARY KEY (model_id, symbol, interval, asof_open_time_ms)
);

CREATE INDEX idx_qlib_pred_lookup
    ON qlib_predictions (model_id, interval, generated_at_ms DESC);
```

`score_percentile` stores the rank-based percentile computed over the full batch at prediction time (see Section 8 confidence mapping).

### 7.2 JSON (Reference Only)

JSON may be used for ad-hoc debugging or offline inspection. It is not the operational store.

Path: `tmp/qlib_signals/latest.json`

Schema:

```json
{
  "generated_at_ms": 1779235200000,
  "model_id": "lightgbm_1h_v1",
  "interval": "1h",
  "horizon_bars": 4,
  "scores": [
    {
      "symbol": "BTCUSDT",
      "score": 0.0123,
      "rank": 1,
      "score_percentile": 0.97,
      "asof_open_time_ms": 1779231600000
    }
  ]
}
```

If JSON is written for debugging, use `os.replace()` (not `os.rename()`) which is atomic on Windows even when the destination exists.

---

## 8. C++ Plugin Design

Add a new plugin:

```text
plugins/src/qlib_model_signal/
  CMakeLists.txt
  strategy_qlib_model_signal.cpp
```

Strategy type:

```text
qlib_model_signal
```

Config sketch:

```json
{
  "name": "Qlib LightGBM 1h Signal",
  "type": "qlib_model_signal",
  "intervals": ["1h"],
  "scan_interval_seconds": 900,
  "max_hold_duration_seconds": 86400,
  "risk_pct": 0.01,
  "sl_multiplier": 1.5,
  "tp_multiplier": 3.0,
  "takeProfitPercent": 20.0,
  "min_notional": 1.0,
  "atr_period": 14,
  "min_confidence": 0.5,
  "params": {
    "source": "sqlite",
    "db_path": "data/qlib_predictions.db",
    "model_id": "lightgbm_1h_v1",
    "max_artifact_age_seconds": 7200,
    "max_data_age_seconds": 3600,
    "long_threshold": 0.003,
    "short_threshold": -0.003,
    "confidence_mode": "rank",
    "min_confidence_percentile": 0.6,
    "dry_run": false,
    "fail_mode": "open"
  }
}
```

**`dry_run: true`** causes the plugin to evaluate scores and log what signal would have been emitted, but always returns `Direction::None`. Use this during Phase 4 observe-only validation on mainnet without placing orders.

**`max_artifact_age_seconds`** checks `now - generated_at_ms`. This verifies the prediction batch itself is recent.

**`max_data_age_seconds`** checks `now - asof_open_time_ms`. This verifies the input candle the prediction is based on is not stale. Both checks must pass; failing either returns `Direction::None`.

Example: for `1h` interval with 15-minute scan cadence, `max_data_age_seconds: 3600` ensures the plugin does not act on a prediction derived from a candle that is more than one bar old.

Signal mapping:

| Condition | Result |
|---|---|
| missing DB and `fail_mode=open` | `Direction::None` |
| missing DB and `fail_mode=closed` | `Direction::None`, warning log |
| `now - generated_at_ms > max_artifact_age_seconds` | `Direction::None` |
| `now - asof_open_time_ms > max_data_age_seconds` | `Direction::None` |
| `dry_run=true` | log signal, return `Direction::None` |
| `score >= long_threshold` | `Direction::Long` |
| `score <= short_threshold` | `Direction::Short` |
| otherwise | `Direction::None` |

**Confidence mapping — rank-based (default):**

LightGBM regression scores are not calibrated probabilities. Absolute score magnitude is not a reliable confidence measure across model versions or market regimes. The preferred method uses the rank percentile computed over the full batch:

```text
confidence = clamp(score_percentile, 0.0, 1.0)
```

`score_percentile` is stored in the DB at prediction time by `predict_latest.py` and represents the signal's standing in the full universe for that batch (e.g., top 5% → 0.95).

When `confidence_mode: "rank"`, the plugin reads `score_percentile` directly. When `confidence_mode: "absolute"`, the legacy formula is used:

```text
confidence = clamp(abs(score) / score_to_confidence_scale, 0.0, 1.0)
```

Use `confidence_mode: "absolute"` only for backward compatibility with test fixtures.

`min_confidence_percentile` filters out signals below a percentile threshold before direction is evaluated (e.g., 0.6 means only top-40% and bottom-40% extremes are considered).

Reason string example:

```text
qlib model=lightgbm_1h_v1 score=0.0123 pct=0.97 rank=1 artifact_age_s=420 data_age_s=1800 dry_run=false
```

The plugin should not:

- run Python inside `evaluate()`
- train models
- query Binance
- place orders
- duplicate C++ risk controls

---

## 9. Runtime Flow

### 9.1 Trigger Contract

`predict_latest.py` must be triggered by an external scheduler (cron, task scheduler, or a watcher script). It must not be spawned from within C++ `evaluate()`.

Recommended trigger window: run `predict_latest.py` within 60 seconds of each candle close for the configured interval. For `1h`, this means triggering at `:01` of each hour.

To signal readiness, `predict_latest.py` writes a completion flag after committing to SQLite:

```text
tmp/qlib_signals/ready_<epoch_ms>.flag
```

The flag is optional for the C++ plugin (the plugin reads the DB directly) but is useful for monitoring and alerting.

### 9.2 Execution Steps

1. External scheduler triggers `predict_latest.py` after fresh candles are available.
2. `predict_latest.py` queries the Binance data, runs inference, computes `score_percentile` over the full batch, and inserts rows into `data/qlib_predictions.db` atomically via a transaction.
3. The C++ bot continues its normal scan cycle.
4. `qlib_model_signal` reads the latest row for `(model_id, symbol, interval)` from the DB.
5. The plugin applies dual stale checks (`max_artifact_age_seconds`, `max_data_age_seconds`).
6. The plugin returns a normal `strategy::Signal` (or `Direction::None` if dry_run or stale).
7. Existing `SignalEngine` applies all live gates and order logic.

### 9.3 Windows Atomic Write Notes

For the SQLite path, atomicity is guaranteed by the SQLite WAL journal mode. No rename is needed.

For any JSON debug output, use `os.replace(src, dst)` rather than `os.rename()`. On Windows, `os.rename()` raises `FileExistsError` if the destination exists while `os.replace()` is atomic.

---

## 10. Alternative Integration Options

### Option A - Qlib Offline Research Only

Use Qlib only to discover features, thresholds, and strategies. Reimplement the final rule in C++.

Pros:

- safest live runtime
- no live Python dependency
- simplest operations

Cons:

- loses model flexibility
- C++ implementation effort grows as model complexity grows

### Option B - Qlib Prediction Sidecar And C++ Plugin

Recommended option.

Pros:

- keeps live runtime small
- supports ML prediction
- fits existing plugin architecture
- easy to disable if unstable

Cons:

- requires data bridge and prediction artifact contract
- requires model staleness handling

### Option C - Call Qlib Python From `evaluate()`

Not recommended.

Problems:

- too slow for scan cycle
- introduces Python dependency into live decision path
- harder timeout and failure behavior
- more likely to block order opportunities

### Option D - Replace C++ Execution With Qlib Backtest/Executor

Not recommended.

Problems:

- current bot already has Binance Futures order implementation
- Qlib backtest/executor is not a drop-in replacement for this bot's futures behavior
- would bypass tested project-specific risk and order controls

---

## 11. Validation Plan

### Phase 1 - Data Spike

Goal:

Confirm Binance OHLCV can be exported, converted to Qlib `.bin`, and queried by Qlib without calendar data loss.

Deliverables:

- `tools/qlib_bridge/export_binance_klines.py`
- `tools/qlib_bridge/requirements.txt` with pinned versions
- sample data for `BTCUSDT`, `ETHUSDT`, and 10 to 20 liquid USDT perpetuals
- Qlib provider under `data/qlib/`
- smoke test that calls `qlib.data.D.features(...)`

Pass criteria:

- no missing OHLC columns
- calendar matches expected candle close/open times
- no rows dropped at `00:00 UTC` crossings or Sunday-to-Monday boundary
- at least one interval works end to end

### Phase 2 - Research Baseline

Goal:

Train a simple LightGBM model and generate historical predictions with validated walk-forward methodology.

Deliverables:

- training script or YAML workflow
- explicit walk-forward config (train window, test window, embargo bars, purging logic)
- model artifact
- metrics report (IC, Rank IC, out-of-sample)
- prediction export script that computes `score_percentile` per batch

Pass criteria:

- predictions have sane score distribution
- no obvious lookahead bug
- IC and Rank IC are measured out of sample with embargo enforced
- `score_percentile` distribution is approximately uniform (sanity check on ranking)

### Phase 3 - Live Signal Plugin

Goal:

Let the C++ bot consume Qlib prediction scores without running Qlib in the scan path.

Deliverables:

- `qlib_model_signal` plugin
- unit tests covering: stale artifact, stale data, missing DB, threshold logic, dry_run mode, rank-based confidence
- config example with `dry_run: true`

Pass criteria:

- plugin returns `None` on missing, stale artifact, or stale data
- plugin returns `None` when `dry_run: true` regardless of score
- plugin returns deterministic Long/Short for fixture predictions
- no Python process is invoked from `evaluate()`

### Phase 4 - Paper Trading

Goal:

Run Qlib signal in `dry_run: true` mode on mainnet and compare logged signals against current rule-based strategies before enabling live orders.

Metrics:

- number of model signals logged (dry_run)
- number of signals that would have been blocked by risk/order/exposure controls
- score vs realized return (look-back on logged signals)
- score age distribution at time of evaluation
- signal overlap with existing strategies
- score distribution drift vs training distribution (mean, std, p10/p90)

Promotion criteria from dry_run to live:

- at minimum 2 weeks of `dry_run` logging
- logged signal hit rate > 50% directionally
- no score distribution drift alert triggered
- realized return on logged signals positive at net of expected spread/funding

---

## 12. Risk Register

| Risk | Impact | Mitigation |
|---|---:|---|
| Lookahead leakage in Qlib labels/features | High | Strict train/valid/test split with embargo, inspect label shift, add leakage tests |
| Calendar mismatch for 24/7 crypto | High | Register custom 24/7 calendar provider, Phase 1 smoke test validates no row drops at midnight and weekend boundary |
| `asof_open_time_ms` stale while `generated_at_ms` is fresh | High | Dual stale check: `max_artifact_age_seconds` + `max_data_age_seconds` |
| Atomic write race on Windows (JSON) | High | Use SQLite as primary store; use `os.replace()` if JSON is written |
| Qlib backtest differs from futures live behavior | Medium | Treat Qlib PnL as research-only, validate live through bot metrics |
| Score distribution drift post-deployment | Medium | Track running stats live (mean, std, p10/p90), alert on drift vs training distribution |
| Absolute score magnitude not calibrated | Medium | Use rank-based `score_percentile` as default confidence mode |
| Python dependency breaks live trading | Medium | Do not invoke Python from C++ `evaluate()` |
| Model overfits short crypto history | Medium | Walk-forward validation with embargo, rank-based metrics, Phase 4 dry_run paper trading |
| Too many signals increase order pressure | Medium | Threshold + `min_confidence_percentile` filter, existing order cap and exposure controls |
| Data export drift from live cache | Medium | Reconcile exported candles against Binance REST and runtime cache samples |
| predict_latest.py trigger race with candle close | Low | Trigger at `:01` past candle close, dual stale check catches late artifacts |

---

## 13. Open Decisions

~~1. Prediction store: start with JSON for MVP or SQLite immediately?~~ **Resolved: SQLite from MVP.**

2. First interval: `1h` is recommended, but `30m` may produce more samples.
3. First universe: top 20 liquid USDT perpetuals or all tradable USDT perpetuals?
4. Score usage: standalone strategy or ML gate for existing rule-based strategies?
5. Retraining cadence: daily, weekly, or manual first?

Recommended defaults:

| Decision | Default |
|---|---|
| Store | **SQLite from MVP** |
| First interval | `1h` |
| Universe | top 20 liquid USDT perpetuals |
| Score usage | standalone strategy first |
| Retraining cadence | manual first, then weekly |

---

## 14. Implementation Checklist

- [ ] Create `tools/qlib_bridge/README.md`
- [ ] Create `tools/qlib_bridge/requirements.txt` with pinned versions
- [ ] Create Binance historical kline exporter
- [ ] Register custom 24/7 Qlib calendar provider
- [ ] Add Qlib environment setup notes (follow Gemini sidecar pattern for venv path)
- [ ] Convert one interval to Qlib `.bin`
- [ ] Add Qlib data smoke test (including midnight and weekend row continuity check)
- [ ] Implement walk-forward validation with embargo and purging
- [ ] Train first LightGBM baseline
- [ ] Export predictions to `data/qlib_predictions.db` with `score_percentile`
- [ ] Create `qlib_model_signal` plugin with dual stale check and rank-based confidence
- [ ] Add plugin unit tests (stale artifact, stale data, dry_run, rank confidence, threshold)
- [ ] Add example config entry with `dry_run: true`
- [ ] Run Phase 4 observe-only validation (minimum 2 weeks)
- [ ] Promote to live after hit rate and distribution drift criteria met

---

## 15. Final Recommendation

Proceed with Option B: Qlib prediction sidecar plus C++ strategy plugin.

Do not replace the C++ live engine. Do not call Qlib from `IStrategy::evaluate()`. Do not use Qlib's Coingecko crypto collector as the production research dataset. Use Binance OHLCV exported from the bot's own data source, train and backtest in Qlib offline, then feed only prediction scores back into the existing strategy plugin path.

Use SQLite as the prediction store from the start. Use rank-based percentile confidence rather than absolute score scaling. Enforce dual stale checks (artifact age and data age) in the plugin. Run in `dry_run` mode for a minimum of 2 weeks before enabling live orders from the Qlib signal.
