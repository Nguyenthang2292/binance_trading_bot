# Microsoft Qlib Integration Plan - v1.0

**Date:** 2026-05-20
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
3. Export model scores into a small local artifact such as SQLite or JSON.
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

The project already has a safe precedent for Python sidecars: `GeminiFilterController` invokes a Python module with JSON input, timeout handling, cache keys, runtime directories, and fail-closed behavior. Qlib integration should follow this sidecar style instead of linking Python or Qlib into the C++ binary.

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
    G --> H["SQLite or latest.json score store"]
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
    README.md
```

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

---

## 7. Prediction Store Contract

Use either SQLite or JSON for MVP.

### 7.1 JSON MVP

Path:

```text
tmp/qlib_signals/latest.json
```

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
      "asof_open_time_ms": 1779231600000
    }
  ]
}
```

### 7.2 SQLite Production Path

Path:

```text
data/qlib_predictions.db
```

Suggested table:

```sql
CREATE TABLE qlib_predictions (
    model_id TEXT NOT NULL,
    symbol TEXT NOT NULL,
    interval TEXT NOT NULL,
    asof_open_time_ms INTEGER NOT NULL,
    generated_at_ms INTEGER NOT NULL,
    score REAL NOT NULL,
    rank INTEGER,
    PRIMARY KEY (model_id, symbol, interval, asof_open_time_ms)
);
```

SQLite is preferred after MVP because it gives atomic writes, easier stale checks, and simpler test fixtures.

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
    "source": "json",
    "path": "tmp/qlib_signals/latest.json",
    "model_id": "lightgbm_1h_v1",
    "max_signal_age_seconds": 7200,
    "long_threshold": 0.003,
    "short_threshold": -0.003,
    "score_to_confidence_scale": 0.02,
    "fail_mode": "open"
  }
}
```

Signal mapping:

| Condition | Result |
|---|---|
| missing file or DB and `fail_mode=open` | `Direction::None` |
| missing file or DB and `fail_mode=closed` | `Direction::None`, warning log |
| stale prediction | `Direction::None` |
| `score >= long_threshold` | `Direction::Long` |
| `score <= short_threshold` | `Direction::Short` |
| otherwise | `Direction::None` |

Confidence mapping:

```text
confidence = clamp(abs(score) / score_to_confidence_scale, 0.0, 1.0)
```

Reason string example:

```text
qlib model=lightgbm_1h_v1 score=0.0123 rank=1 age_s=420
```

The plugin should not:

- run Python inside `evaluate()`
- train models
- query Binance
- place orders
- duplicate C++ risk controls

---

## 9. Runtime Flow

1. `predict_latest.py` runs after fresh candles are available.
2. It updates the latest prediction artifact atomically.
3. The C++ bot continues its normal scan cycle.
4. `qlib_model_signal` reads the latest artifact.
5. The plugin returns a normal `strategy::Signal`.
6. Existing `SignalEngine` applies all live gates and order logic.

Atomic write pattern for JSON:

```text
write latest.json.tmp
fsync if practical
rename latest.json.tmp -> latest.json
```

This avoids the C++ plugin reading a partially written file.

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

Confirm Binance OHLCV can be exported, converted to Qlib `.bin`, and queried by Qlib.

Deliverables:

- `tools/qlib_bridge/export_binance_klines.py`
- sample data for `BTCUSDT`, `ETHUSDT`, and 10 to 20 liquid USDT perpetuals
- Qlib provider under `data/qlib/`
- smoke test that calls `qlib.data.D.features(...)`

Pass criteria:

- no missing OHLC columns
- calendar matches expected candle close/open times
- at least one interval works end to end

### Phase 2 - Research Baseline

Goal:

Train a simple LightGBM model and generate historical predictions.

Deliverables:

- training script or YAML workflow
- model artifact
- metrics report
- prediction export script

Pass criteria:

- predictions have sane score distribution
- no obvious lookahead bug
- IC and Rank IC are measured out of sample

### Phase 3 - Live Signal Plugin

Goal:

Let the C++ bot consume Qlib prediction scores without running Qlib in the scan path.

Deliverables:

- `qlib_model_signal` plugin
- unit tests for stale/missing/threshold behavior
- config example

Pass criteria:

- plugin returns `None` on missing or stale predictions
- plugin returns deterministic Long/Short for fixture predictions
- no Python process is invoked from `evaluate()`

### Phase 4 - Paper Trading

Goal:

Run Qlib signal in testnet or observe-only mode and compare against current rule-based strategies.

Metrics:

- number of model signals
- number of signals blocked by risk/order/exposure controls
- score vs realized return
- score age distribution
- signal overlap with existing strategies
- realized trade outcome if enabled

---

## 12. Risk Register

| Risk | Impact | Mitigation |
|---|---:|---|
| Lookahead leakage in Qlib labels/features | High | Strict train/valid/test split, inspect label shift, add leakage tests |
| Calendar mismatch for 24/7 crypto | High | Use Binance candle timestamps, avoid stock-market session processors in MVP |
| Qlib backtest differs from futures live behavior | Medium | Treat Qlib PnL as research-only, validate live through bot metrics |
| Prediction artifact is stale | Medium | `max_signal_age_seconds`, generated timestamp, fail to `None` |
| Python dependency breaks live trading | Medium | Do not invoke Python from C++ `evaluate()` |
| Model overfits short crypto history | Medium | Walk-forward validation, rank-based metrics, paper trading |
| Too many signals increase order pressure | Medium | Use thresholds, rank filters, existing order cap and exposure controls |
| Data export drift from live cache | Medium | Reconcile exported candles against Binance REST and runtime cache samples |

---

## 13. Open Decisions

1. Prediction store: start with JSON for MVP or SQLite immediately?
2. First interval: `1h` is recommended, but `30m` may produce more samples.
3. First universe: top 20 liquid USDT perpetuals or all tradable USDT perpetuals?
4. Score usage: standalone strategy or ML gate for existing rule-based strategies?
5. Retraining cadence: daily, weekly, or manual first?

Recommended defaults:

| Decision | Default |
|---|---|
| Store | JSON MVP, SQLite after plugin validation |
| First interval | `1h` |
| Universe | top 20 liquid USDT perpetuals |
| Score usage | standalone strategy first |
| Retraining cadence | manual first, then weekly |

---

## 14. Implementation Checklist

- [ ] Create `tools/qlib_bridge/README.md`
- [ ] Create Binance historical kline exporter
- [ ] Add Qlib environment setup notes
- [ ] Convert one interval to Qlib `.bin`
- [ ] Add Qlib data smoke test
- [ ] Train first LightGBM baseline
- [ ] Export `latest.json` predictions
- [ ] Create `qlib_model_signal` plugin
- [ ] Add plugin unit tests
- [ ] Add example config entry
- [ ] Run observe-only or testnet validation
- [ ] Decide whether to move prediction store to SQLite

---

## 15. Final Recommendation

Proceed with Option B: Qlib prediction sidecar plus C++ strategy plugin.

Do not replace the C++ live engine. Do not call Qlib from `IStrategy::evaluate()`. Do not use Qlib's Coingecko crypto collector as the production research dataset. Use Binance OHLCV exported from the bot's own data source, train and backtest in Qlib offline, then feed only prediction scores back into the existing strategy plugin path.

