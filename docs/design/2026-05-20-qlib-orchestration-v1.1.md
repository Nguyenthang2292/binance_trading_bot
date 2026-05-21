# Qlib Orchestration Layer Design - v1.1

**Date:** 2026-05-20
**Revision:** v1.1 - addresses orchestration review findings
**Status:** PROPOSED
**Audience:** AI agents, human developers

**Related project areas:**
- `docs/design/2026-05-20-qlib-integration-v1.0.md` - parent Qlib integration design
- `plugins/src/qlib_model_signal/strategy_qlib_model_signal.cpp` - C++ plugin that consumes predictions
- `src/engine/signal_engine.cpp` - live and shadow signal gate path
- `src/scanner/market_scanner.cpp` - candle-close source
- `tools/qlib_bridge/export_binance_klines.py` - historical export script
- `tools/qlib_bridge/train_workflow.py` - walk-forward training script
- `tools/qlib_bridge/predict_latest.py` - prediction writer
- `config.json` - static runtime configuration

---

## 1. Executive Summary

The Qlib integration has four operational phases: export, train, predict, and promote from shadow trading to live trading. This revision keeps orchestration embedded in the C++ bot, but fixes the unsafe parts of the earlier design:

- `qlib_model_signal` is configured statically at startup; the orchestrator does **not** inject strategy config at runtime.
- Mutable runtime state lives in SQLite and is reloaded by the bot through a small state store, not by rewriting `config.json`.
- Phase 3 refreshes the just-closed candle before inference so `predict_latest.py --asof-ms` always has input data.
- Phase 2 publishes models through versioned artifacts and an atomic active-manifest swap; it never overwrites the active model in place.
- Shadow metrics are recorded inside the SignalEngine gate path, so promotion uses what the bot would really have traded after filters, risk, exposure, order-cap, and Gemini gates.
- Actual returns are stored in SQLite after the configured prediction horizon matures; promotion never estimates outcomes from prediction rows alone.
- Promotion is still automatic, but it is guarded by at least two weeks of mature shadow metrics, drift checks, net-return checks, and automatic rollback.

Python is launched by the orchestrator through `CreateProcess` on Windows. Python is still never executed from `IStrategy::evaluate()`.

---

## 2. Understanding Summary

| Item | Decision |
|------|----------|
| Trigger mechanism | Embedded scheduler in the C++ bot |
| Phase 1+2 schedule | 07:00 Monday-Friday, linked export -> train -> publish pipeline |
| Phase 3 schedule | Per candle close, after a finalization delay |
| Phase 3 data freshness | Refresh latest closed candles before prediction |
| Phase 4 | Automatic shadow -> live_canary -> live, with automatic rollback |
| Static config | Loaded at startup; no runtime strategy injection |
| Mutable state | SQLite WAL runtime-state tables |
| Model activation | Versioned model artifacts plus atomic active manifest |
| Failure policy | Retry with exponential backoff, then skip + log |
| Platform | Windows, single machine |

**Non-goals (v1.1):**
- No external orchestration tools such as Airflow, Celery, or Task Scheduler.
- No Python/Qlib execution inside `IStrategy::evaluate()`.
- No in-place overwrite of the active model file.
- No promotion by editing `config.json` and hoping the bot reloads it.
- No live promotion from raw predictions without shadow metrics and matured actual returns.

---

## 3. Assumptions

1. The market data layer can notify candle close with `symbol`, `interval`, `open_time_ms`, and `close_time_ms`, or the scheduler can derive those values from `KlineCache`.
2. `python_exe`, script paths, model directories, and DB paths are configurable.
3. `qlib_model_signal` is present in `strategies[]` before startup. It starts in `shadow` mode from SQLite state.
4. Phase 1 historical export completes in under 15 minutes on weekday mornings.
5. Phase 2 LightGBM training completes in under 30 minutes in normal operation.
6. Phase 3 refresh + inference completes inside the candle interval. For `1h`, target is under 60 seconds.
7. `train_workflow.py` uses walk-forward validation with embargo and writes both a model and report.
8. Actual-return computation uses the same `horizon_bars` used by training and prediction.
9. SignalEngine will add a shadow execution path before this design is enabled for promotion.

---

## 4. Decision Log

| # | Decision | Alternatives Considered | Rationale |
|---|----------|-------------------------|-----------|
| 1 | Embed scheduler in C++ bot | External watcher process | Bot already has candle-close context and process lifecycle control. |
| 2 | Use separate batch and candle scheduler threads | Single orchestrator queue | Training must not delay per-candle inference. |
| 3 | Do not hold the ProcessManager mutex while waiting on children | Serialize all subprocesses | Serialization would let Phase 2 block Phase 3. |
| 4 | Refresh latest candle before Phase 3 predict | Predict from daily export only | Per-candle prediction needs the just-closed bar in the dataset. |
| 5 | Preconfigure qlib strategy at startup | Runtime strategy injection | Current catalog is initialized at startup; injection requires a broader reload design. |
| 6 | Store mutable runtime state in SQLite | Rewrite `config.json` on promotion | SQLite state survives restart and can be polled atomically without rebuilding the catalog. |
| 7 | Use shadow metrics from SignalEngine | Infer promotion quality from raw scores | Promotion must reflect the same gates used before live order placement. |
| 8 | Store actual returns after horizon maturity | Compute returns from predictions table | Predictions do not contain candles or realized returns. |
| 9 | Publish models by active manifest | Overwrite `model.txt` directly | Prediction sees either the old complete model or the new complete model, never a partial file. |
| 10 | Automatic rollback from live to shadow | No rollback | Model drift and stale data are expected operational failures. |

---

## 5. Architecture

```text
C++ bot process
|
|-- BatchSchedulerThread
|   |-- Phase 1 historical export
|   |-- Phase 2 train_workflow.py
|   `-- ModelPublisher
|       |-- validates staging artifact
|       |-- writes versioned model run
|       `-- atomically swaps active manifest
|
|-- CandleSchedulerThread
|   |-- LatestCandleRefresher
|   |   |-- fetches or reads the just-closed candle
|   |   |-- upserts dataset
|   |   `-- upserts qlib_candles
|   |-- ActualReturnUpdater
|   |-- predict_latest.py using active model manifest
|   `-- PromotionController
|
|-- QlibStateStore
|   `-- reloads execution_mode and active model state from SQLite
|
|-- SignalEngine
|   |-- qlib_model_signal returns candidate direction
|   |-- normal filters/risk/exposure/order-cap/Gemini gates run
|   |-- ShadowMetricsRecorder writes would-trade outcome
|   `-- order placement is suppressed unless state is live/live_canary
|
`-- SQLite WAL: data/qlib_predictions.db
    |-- qlib_runtime_state
    |-- qlib_job_runs
    |-- qlib_model_runs
    |-- qlib_candles
    |-- qlib_predictions
    |-- qlib_actual_returns
    |-- qlib_shadow_signals
    |-- qlib_shadow_outcomes
    `-- qlib_promotion_evaluations
```

Primary boundary:

- Qlib owns data preparation, model training, and prediction scores.
- The C++ bot owns live/shadow execution decisions, risk gates, order gates, and promotion state.

---

## 6. Component Design

### 6.1 OrchestratorConfig

Loaded from the `qlib_orchestration` key in `config.json` at startup.

```cpp
struct OrchestratorConfig {
    bool enabled{false};

    std::string pythonExe;
    std::string scriptsDir{"tools/qlib_bridge"};
    std::string datasetPath{"data/qlib/klines_1h.parquet"};
    std::string dbPath{"data/qlib_predictions.db"};
    std::string modelId{"lightgbm_1h_v1"};
    std::string interval{"1h"};
    std::vector<std::string> symbols;

    // Training and model publishing.
    int horizonBars{4};
    std::string modelStagingDir{"data/qlib/model_staging"};
    std::string modelArtifactsDir{"data/qlib/model_artifacts"};
    std::string modelManifestPath{"data/qlib/current/lightgbm_1h_v1.json"};
    std::string reportDir{"data/qlib/reports"};

    // Batch schedule.
    int batchHour{7};
    int batchMinute{0};
    std::vector<int> batchWeekdays{0, 1, 2, 3, 4}; // 0=Mon..4=Fri

    // Candle schedule.
    int candleFinalizeDelaySeconds{60};
    int candleCloseDebounceSeconds{10};

    // Retry.
    int maxAttempts{3};
    int backoffBaseSeconds{30};

    // State reload.
    int stateReloadSeconds{5};

    // Shadow/promotion.
    int promotionMinShadowDays{14};
    int promotionMinCandles{336};       // 2 weeks @ 1h
    int promotionMinMatureSignals{100};
    double promotionMinSharpe{0.5};
    double promotionMinHitRate{0.52};
    double promotionMinMeanNetReturnBps{0.0};
    double promotionMaxDriftZ{3.0};
    double promotionMaxStalePredictionRatio{0.02};

    // Live canary and rollback.
    int canaryDays{3};
    double canaryRiskMultiplier{0.25};
    double rollbackMinRollingSharpe{0.0};
    double rollbackMaxDrawdownBps{200.0};
    int rollbackLookbackCandles{168};

    // Cost model for net returns.
    double estimatedRoundTripFeeBps{8.0};
    double estimatedSlippageBps{2.0};
    double estimatedFundingBpsPerDay{0.0};
};
```

### 6.2 SQLite State And Metrics Schema

SQLite is the source of truth for mutable orchestration state. Use WAL mode and set `busy_timeout` for every reader and writer.

```sql
CREATE TABLE IF NOT EXISTS qlib_runtime_state (
    model_id             TEXT NOT NULL,
    interval             TEXT NOT NULL,
    execution_mode       TEXT NOT NULL CHECK (execution_mode IN ('disabled','shadow','live_canary','live')),
    active_run_id        TEXT,
    active_manifest_path TEXT,
    state_version        INTEGER NOT NULL DEFAULT 0,
    promoted_at_ms       INTEGER,
    rollback_reason      TEXT,
    updated_at_ms        INTEGER NOT NULL,
    PRIMARY KEY (model_id, interval)
);

CREATE TABLE IF NOT EXISTS qlib_job_runs (
    job_id          TEXT PRIMARY KEY,
    job_type        TEXT NOT NULL,
    schedule_key    TEXT NOT NULL,
    status          TEXT NOT NULL CHECK (status IN ('running','succeeded','failed','stale')),
    pid             INTEGER,
    started_at_ms   INTEGER NOT NULL,
    completed_at_ms INTEGER,
    exit_code       INTEGER,
    log_path        TEXT,
    error           TEXT
);

CREATE TABLE IF NOT EXISTS qlib_model_runs (
    run_id              TEXT PRIMARY KEY,
    model_id            TEXT NOT NULL,
    interval            TEXT NOT NULL,
    horizon_bars        INTEGER NOT NULL,
    model_path          TEXT NOT NULL,
    manifest_path       TEXT NOT NULL,
    report_path         TEXT NOT NULL,
    feature_schema_hash TEXT NOT NULL,
    dataset_fingerprint TEXT NOT NULL,
    oos_ic              REAL,
    oos_rank_ic         REAL,
    oos_rows            INTEGER,
    trained_at_ms       INTEGER NOT NULL,
    published_at_ms     INTEGER,
    status              TEXT NOT NULL CHECK (status IN ('staged','active','rejected','retired'))
);

CREATE TABLE IF NOT EXISTS qlib_candles (
    symbol            TEXT NOT NULL,
    interval          TEXT NOT NULL,
    open_time_ms      INTEGER NOT NULL,
    close_time_ms     INTEGER NOT NULL,
    open              REAL NOT NULL,
    high              REAL NOT NULL,
    low               REAL NOT NULL,
    close             REAL NOT NULL,
    volume            REAL NOT NULL,
    quote_volume      REAL,
    trade_count       INTEGER,
    inserted_at_ms    INTEGER NOT NULL,
    PRIMARY KEY (symbol, interval, open_time_ms)
);

CREATE TABLE IF NOT EXISTS qlib_predictions (
    model_id            TEXT    NOT NULL,
    run_id              TEXT,
    symbol              TEXT    NOT NULL,
    interval            TEXT    NOT NULL,
    asof_open_time_ms   INTEGER NOT NULL,
    generated_at_ms     INTEGER NOT NULL,
    horizon_bars        INTEGER NOT NULL,
    score               REAL    NOT NULL,
    rank                INTEGER,
    score_percentile    REAL,
    PRIMARY KEY (model_id, symbol, interval, asof_open_time_ms)
);

CREATE INDEX IF NOT EXISTS idx_qlib_pred_lookup
    ON qlib_predictions (model_id, interval, generated_at_ms DESC);

CREATE TABLE IF NOT EXISTS qlib_actual_returns (
    symbol              TEXT NOT NULL,
    interval            TEXT NOT NULL,
    asof_open_time_ms   INTEGER NOT NULL,
    horizon_bars        INTEGER NOT NULL,
    exit_open_time_ms   INTEGER NOT NULL,
    entry_close         REAL NOT NULL,
    exit_close          REAL NOT NULL,
    raw_return          REAL NOT NULL,
    computed_at_ms      INTEGER NOT NULL,
    PRIMARY KEY (symbol, interval, asof_open_time_ms, horizon_bars)
);

CREATE TABLE IF NOT EXISTS qlib_shadow_signals (
    shadow_id           TEXT PRIMARY KEY,
    model_id            TEXT NOT NULL,
    run_id              TEXT,
    symbol              TEXT NOT NULL,
    interval            TEXT NOT NULL,
    asof_open_time_ms   INTEGER NOT NULL,
    generated_at_ms     INTEGER NOT NULL,
    score               REAL NOT NULL,
    score_percentile    REAL,
    direction           TEXT NOT NULL CHECK (direction IN ('long','short','none')),
    confidence          REAL NOT NULL,
    execution_mode      TEXT NOT NULL,
    blocked_stage       TEXT,          -- null means would place order
    would_place_order   INTEGER NOT NULL,
    current_price       REAL,
    atr                 REAL,
    reason              TEXT,
    captured_at_ms      INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS qlib_shadow_outcomes (
    shadow_id           TEXT PRIMARY KEY,
    raw_return          REAL NOT NULL,
    direction_return    REAL NOT NULL,
    net_return          REAL NOT NULL,
    hit                 INTEGER NOT NULL,
    matured_at_ms       INTEGER NOT NULL,
    FOREIGN KEY (shadow_id) REFERENCES qlib_shadow_signals(shadow_id)
);

CREATE TABLE IF NOT EXISTS qlib_promotion_evaluations (
    eval_id             TEXT PRIMARY KEY,
    model_id            TEXT NOT NULL,
    interval            TEXT NOT NULL,
    evaluated_at_ms     INTEGER NOT NULL,
    execution_mode      TEXT NOT NULL,
    mature_signals      INTEGER NOT NULL,
    candles             INTEGER NOT NULL,
    hit_rate            REAL,
    sharpe              REAL,
    mean_net_return_bps REAL,
    drift_z             REAL,
    stale_ratio         REAL,
    decision            TEXT NOT NULL,
    reason              TEXT
);
```

### 6.3 ProcessManager

`ProcessManager` starts Python subprocesses with retry, timeout, log redirection, and job tracking.

Key requirements:

- Do not hold the `ProcessManager` mutex while waiting for a child process. Use locks only for child registry and log file writes.
- Use `CreateProcessW` with explicit argument quoting and a configured working directory.
- Redirect stdout/stderr to `logs/qlib_orch_YYYY-MM-DD.log`.
- Put each subprocess in a Windows Job Object so timeout or bot shutdown kills the process tree.
- Record every attempt in `qlib_job_runs`.
- Treat timeout as failure and retry according to `maxAttempts`.

### 6.4 BatchSchedulerThread (Phase 1 -> Phase 2 -> Publish)

Batch jobs are idempotent by `(job_type, schedule_key)`, where `schedule_key` is usually `YYYY-MM-DD`.

Main loop:

```text
every 60 seconds:
    if today is not a configured weekday: continue
    if local time is before batch schedule: continue
    if qlib_job_runs has succeeded batch job for today: continue
    if qlib_job_runs has running non-stale batch job for today: continue

    create running batch job
    run Phase 1 historical export
    if Phase 1 fails: mark failed and stop

    run Phase 2 training into staging directory
    if Phase 2 fails: mark failed and keep active model unchanged

    ModelPublisher validates staging output
    if publish succeeds: update qlib_runtime_state.active_run_id and active_manifest_path
    mark batch job succeeded
```

**Phase 1 command:**

```text
<pythonExe>
  tools/qlib_bridge/export_binance_klines.py
  --symbols BTCUSDT ETHUSDT ...
  --interval <interval>
  --start-ms <epoch_ms_of_lookback_window_start>
  --end-ms <latest_closed_candle_close_ms>
  --output <datasetPath>
  --convert-mode incremental
  --dump-bin-script <qlib_dump_bin_path>
  --qlib-dir <qlib_provider_dir>
```

**Phase 2 command:**

```text
<pythonExe>
  tools/qlib_bridge/train_workflow.py
  --dataset <datasetPath>
  --interval <interval>
  --horizon-bars <horizonBars>
  --model-out <modelStagingDir>/<run_id>/model.txt
  --report-out <modelStagingDir>/<run_id>/report.json
```

The previous v1.0 command was incomplete because `train_workflow.py` requires both `--horizon-bars` and `--report-out`.

### 6.5 ModelPublisher

`ModelPublisher` owns atomic model activation.

Publish algorithm:

```text
1. Train into a unique staging directory: model_staging/<run_id>/.
2. Verify the model file exists and can be loaded by LightGBM.
3. Verify report.json exists and contains walk-forward metrics.
4. Compute feature_schema_hash and dataset_fingerprint.
5. Move/copy the staging directory to:
   model_artifacts/<model_id>/<run_id>/
6. Write manifest to <modelManifestPath>.tmp:
   {
     "model_id": "...",
     "run_id": "...",
     "interval": "1h",
     "horizon_bars": 4,
     "model_path": "data/qlib/model_artifacts/.../model.txt",
     "report_path": "data/qlib/model_artifacts/.../report.json",
     "feature_schema_hash": "...",
     "dataset_fingerprint": "...",
     "published_at_ms": 1779235200000
   }
7. Atomically replace <modelManifestPath> with the tmp file.
8. Upsert qlib_model_runs and qlib_runtime_state.
```

`predict_latest.py` reads the active manifest or receives the resolved active `model_path`, `run_id`, and `horizon_bars` from the orchestrator. Prediction must never read a model path being written by training.

### 6.6 CandleSchedulerThread (Phase 3)

The candle scheduler processes an interval/asof once, even if many symbols report close events. It stores pending work as a set or queue keyed by `(interval, asof_open_time_ms)`, not as a single `m_pendingCandleMs`.

Callback contract:

```cpp
void notifyCandleClose(
    std::string_view symbol,
    std::string_view interval,
    int64_t openTimeMs,
    int64_t closeTimeMs);
```

Phase 3 flow:

```text
1. Wait candleFinalizeDelaySeconds after close.
2. Debounce symbol-level close events for the same interval/asof.
3. Run LatestCandleRefresher for all configured symbols.
4. Run ActualReturnUpdater for matured horizons.
5. Resolve active model manifest from qlib_runtime_state.
6. Run predict_latest.py for the asof timestamp.
7. Run PromotionController.evaluate().
```

**Latest candle refresh command:**

Implement `tools/qlib_bridge/refresh_latest_candles.py` or extend `export_binance_klines.py` with equivalent behavior.

```text
<pythonExe>
  tools/qlib_bridge/refresh_latest_candles.py
  --symbols BTCUSDT ETHUSDT ...
  --interval <interval>
  --asof-ms <candleOpenTimeMs>
  --dataset <datasetPath>
  --db-path <dbPath>
  --merge-mode upsert
```

Requirements:

- Fetch or read the exact just-closed candle for every configured symbol.
- Upsert the candle into the dataset used by `predict_latest.py`.
- Upsert the same candle into `qlib_candles`.
- Fail Phase 3 before prediction if any required symbol/asof row is missing.

**Prediction command:**

```text
<pythonExe>
  tools/qlib_bridge/predict_latest.py
  --dataset <datasetPath>
  --model-path <active_model_path_from_manifest>
  --model-id <modelId>
  --run-id <active_run_id>
  --horizon-bars <horizonBars>
  --interval <interval>
  --db-path <dbPath>
  --asof-ms <candleOpenTimeMs>
  --ready-dir tmp/qlib_signals
  --debug-json data/qlib/debug_latest.json
```

`predict_latest.py` must store `run_id` and `horizon_bars` in `qlib_predictions`.

### 6.7 ShadowMetricsRecorder

The previous `dry_run=true` behavior is not sufficient for promotion because the plugin returns `Direction::None` and SignalEngine exits before candidate logging and gates.

Required runtime behavior:

1. The qlib plugin returns the candidate `Long` or `Short` when prediction data is eligible.
2. SignalEngine checks `QlibStateStore.execution_mode`.
3. If mode is `shadow`, SignalEngine runs the same pre-order gates it would run for live trading:
   - confidence threshold
   - ATR fallback
   - tracked-position de-duplication
   - risk analytics
   - order cap
   - exposure control
   - Gemini filter if enabled
4. SignalEngine writes one `qlib_shadow_signals` row with the final `blocked_stage` and `would_place_order` result.
5. If mode is `shadow`, SignalEngine suppresses order placement after recording.
6. If mode is `live_canary`, SignalEngine places orders only after applying `canaryRiskMultiplier`.
7. If mode is `live`, SignalEngine uses normal sizing.

This makes promotion metrics reflect the actual bot decision path rather than raw model score quality.

### 6.8 ActualReturnUpdater

`ActualReturnUpdater` runs after every successful latest-candle refresh.

Algorithm:

```text
for each qlib_shadow_signals row without qlib_shadow_outcomes:
    exit_open_time_ms = asof_open_time_ms + horizonBars * interval_ms
    if entry candle and exit candle both exist in qlib_candles:
        raw_return = exit_close / entry_close - 1
        direction_return = raw_return for Long, -raw_return for Short
        net_return = direction_return
                     - estimatedRoundTripFeeBps / 10000
                     - estimatedSlippageBps / 10000
                     - estimatedFundingCostForHoldingPeriod
        hit = direction_return > 0
        insert qlib_shadow_outcomes

for each candle/asof/horizon without qlib_actual_returns:
    if exit candle exists:
        insert raw actual return
```

Rules:

- Never compute an outcome until the exit candle has closed and passed the finalization delay.
- Use the configured `horizon_bars`; do not hardcode `t+1`.
- Promotion uses `qlib_shadow_outcomes.net_return`, not `score * return`.

### 6.9 PromotionController

Promotion is automatic but stateful and reversible.

State machine:

```text
disabled
  -> shadow
  -> live_canary
  -> live

live_canary -> shadow  (automatic rollback)
live        -> shadow  (automatic rollback)
```

Promotion from `shadow` to `live_canary` requires all of:

- Static strategy config exists and state source is SQLite.
- Active model run has a valid walk-forward report with embargo/purging enabled.
- At least `promotionMinShadowDays` elapsed since shadow collection started.
- At least `promotionMinCandles` actual-return candles are available.
- At least `promotionMinMatureSignals` shadow outcomes are available.
- `hit_rate >= promotionMinHitRate`.
- `sharpe(net_return) >= promotionMinSharpe`.
- `mean(net_return) * 10000 >= promotionMinMeanNetReturnBps`.
- Score distribution drift versus training report is within `promotionMaxDriftZ`.
- Stale/missing prediction ratio is <= `promotionMaxStalePredictionRatio`.

Promotion from `live_canary` to `live` requires:

- `canaryDays` elapsed.
- No automatic rollback condition triggered.
- Rolling net returns remain non-negative after costs.
- Prediction freshness remains within configured stale limits.

Rollback from `live_canary` or `live` to `shadow` occurs automatically on:

- Prediction freshness breach for more than one candle.
- Rolling Sharpe below `rollbackMinRollingSharpe`.
- Rolling drawdown worse than `rollbackMaxDrawdownBps`.
- Score distribution drift breach.
- Model manifest missing or active model cannot be loaded by prediction.
- SQLite state DB unavailable for longer than one scan cycle.

State changes update `qlib_runtime_state.execution_mode`; they do not edit `config.json`.

### 6.10 QlibStateStore And Runtime Reload

Static config remains startup-only. Mutable state reload is handled by `QlibStateStore`.

Requirements:

- Load `qlib_runtime_state` at startup.
- If no row exists, initialize `(model_id, interval)` as `execution_mode='shadow'`.
- Refresh state every `stateReloadSeconds` into an atomic in-memory snapshot.
- SignalEngine reads the atomic snapshot when processing qlib signals.
- If state reload fails, fail closed to `shadow` or `disabled`; never assume `live`.
- On restart, resume from SQLite state.

This replaces the v1.0 assumption that the bot hot-reloads `config.json`.

---

## 7. Configuration Schema

Add to `config.json`:

```json
{
  "qlib_orchestration": {
    "enabled": true,
    "python_exe": "C:/Users/Admin/miniconda3/envs/qlib/python.exe",
    "scripts_dir": "tools/qlib_bridge",
    "dataset_path": "data/qlib/klines_1h.parquet",
    "db_path": "data/qlib_predictions.db",
    "model_id": "lightgbm_1h_v1",
    "interval": "1h",
    "symbols": ["BTCUSDT", "ETHUSDT"],
    "horizon_bars": 4,
    "model_staging_dir": "data/qlib/model_staging",
    "model_artifacts_dir": "data/qlib/model_artifacts",
    "model_manifest_path": "data/qlib/current/lightgbm_1h_v1.json",
    "report_dir": "data/qlib/reports",
    "batch_schedule": {
      "hour": 7,
      "minute": 0,
      "weekdays": [0, 1, 2, 3, 4]
    },
    "candle_schedule": {
      "finalize_delay_seconds": 60,
      "debounce_seconds": 10
    },
    "retry": {
      "max_attempts": 3,
      "backoff_base_seconds": 30
    },
    "state": {
      "reload_seconds": 5
    },
    "promotion": {
      "min_shadow_days": 14,
      "min_candles": 336,
      "min_mature_signals": 100,
      "min_sharpe": 0.5,
      "min_hit_rate": 0.52,
      "min_mean_net_return_bps": 0.0,
      "max_drift_z": 3.0,
      "max_stale_prediction_ratio": 0.02
    },
    "canary": {
      "days": 3,
      "risk_multiplier": 0.25
    },
    "rollback": {
      "lookback_candles": 168,
      "min_rolling_sharpe": 0.0,
      "max_drawdown_bps": 200.0
    },
    "cost_model": {
      "estimated_round_trip_fee_bps": 8.0,
      "estimated_slippage_bps": 2.0,
      "estimated_funding_bps_per_day": 0.0
    }
  }
}
```

`qlib_model_signal` must be present in `strategies[]` at startup. It is no longer injected after Phase 3.

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
  "min_confidence": 0.0,
  "execution": {
    "mode_source": "sqlite",
    "default_mode": "shadow",
    "state_db_path": "data/qlib_predictions.db"
  },
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
    "fail_mode": "open",
    "score_to_confidence_scale": 0.01
  }
}
```

Do not use legacy `params.dry_run=true` for promotion metrics, because that mode returns `Direction::None` before SignalEngine can record shadow gate results.

---

## 8. Phase Lifecycle State Machine

```text
Bot starts
|
|-- qlib_orchestration.enabled = false
|   `-- no orchestrator threads
|
`-- qlib_orchestration.enabled = true
    |
    |-- initialize SQLite schema
    |-- initialize qlib_runtime_state if missing: execution_mode=shadow
    |-- start QlibStateStore reload loop
    |-- start BatchSchedulerThread
    |   `-- 07:00 weekday -> export -> train -> atomic publish
    |
    `-- start CandleSchedulerThread
        `-- candle close + delay
            |-- refresh latest candles
            |-- compute matured actual returns
            |-- predict using active manifest
            |-- SignalEngine records shadow metrics during normal scans
            `-- PromotionController.evaluate()
                |
                |-- NotEnoughData -> stay shadow
                |-- BelowThreshold -> stay shadow
                |-- PromotedCanary -> execution_mode=live_canary
                |-- PromotedLive -> execution_mode=live
                `-- RolledBack -> execution_mode=shadow
```

---

## 9. Thread Safety

| Resource | Owner | Protection |
|----------|-------|------------|
| Child process registry | ProcessManager | mutex, not held while waiting |
| Orchestrator logs | ProcessManager | log mutex |
| Job run state | SQLite | WAL transaction + busy timeout |
| Active model selection | ModelPublisher | atomic manifest replace + SQLite state transaction |
| Model artifacts | ModelPublisher | immutable versioned run directories |
| Pending candle work | CandleSchedulerThread + market data callback | mutex + condition_variable + deduped queue |
| Runtime state snapshot | QlibStateStore | atomic shared snapshot |
| Shadow metrics | SignalEngine | SQLite transaction |
| Actual returns | ActualReturnUpdater | SQLite transaction |
| Promotion state | PromotionController | SQLite transaction with state_version compare-and-swap |

---

## 10. Error Handling

| Scenario | Behavior |
|----------|----------|
| Phase 1 historical export fails | Retry; mark job failed; keep active model unchanged |
| Phase 2 training fails | Retry; mark job failed; keep active model manifest unchanged |
| Model validation fails | Reject run; keep previous active manifest |
| Atomic manifest replace fails | Log and retry next batch; previous active model remains active |
| Latest candle refresh misses a symbol/asof | Skip Phase 3 for that asof; log `[CANDLE][REFRESH][SKIP]` |
| Phase 3 predict has no active model | Skip prediction; stay shadow |
| Phase 3 predict fails | Retry; stale prediction ratio may later block promotion or trigger rollback |
| Actual returns not mature | Leave outcomes pending; promotion returns NotEnoughData |
| Shadow metrics write fails | Fail closed for qlib signal in that cycle; log error |
| State reload fails | Treat qlib execution mode as shadow/disabled, never live |
| Promotion below threshold | Write evaluation row; no state change |
| Rollback threshold breached | Atomically set execution_mode=shadow and log reason |
| Bot restart mid job | Job ledger marks old running jobs stale after timeout; Job Object handles child cleanup on normal shutdown |

---

## 11. Logging

Log file: `logs/qlib_orch_YYYY-MM-DD.log`

Examples:

```text
[BATCH][PHASE1][START]      2026-05-20T07:00:01Z symbols=BTCUSDT,ETHUSDT interval=1h
[BATCH][PHASE1][SUCCESS]    2026-05-20T07:04:33Z rows=8640 dataset=data/qlib/klines_1h.parquet
[BATCH][PHASE2][START]      2026-05-20T07:04:34Z run_id=lightgbm_1h_v1_20260520T000434Z horizon_bars=4
[BATCH][PUBLISH][SUCCESS]   2026-05-20T07:11:22Z run_id=... manifest=data/qlib/current/lightgbm_1h_v1.json
[CANDLE][REFRESH][START]    2026-05-20T08:01:00Z asof_ms=1779235200000 symbols=2
[CANDLE][REFRESH][SUCCESS]  2026-05-20T08:01:02Z upserted=2
[CANDLE][PHASE3][SUCCESS]   2026-05-20T08:01:07Z predictions=2 run_id=...
[SHADOW][SIGNAL]            2026-05-20T08:15:00Z symbol=BTCUSDT dir=Long blocked_stage=exposure would_place_order=false
[PHASE4][CHECK]             2026-05-27T14:01:00Z mature=86 sharpe=n/a hit=n/a decision=NotEnoughData
[PHASE4][PROMOTE_CANARY]    2026-06-03T14:01:00Z mature=214 sharpe=0.73 hit=0.55 mean_net_bps=1.2
[PHASE4][ROLLBACK]          2026-06-05T09:01:00Z from=live_canary to=shadow reason=drift_z_exceeded
```

Python stdout/stderr is appended under the owning job attempt.

---

## 12. New File Structure

```text
src/
`-- orchestration/
    |-- orchestrator_config.h/.cpp
    |-- process_manager.h/.cpp
    |-- qlib_state_store.h/.cpp
    |-- model_publisher.h/.cpp
    |-- batch_scheduler_thread.h/.cpp
    |-- candle_scheduler_thread.h/.cpp
    |-- actual_return_updater.h/.cpp
    |-- promotion_controller.h/.cpp
    `-- shadow_metrics_recorder.h/.cpp

tools/
`-- qlib_bridge/
    |-- refresh_latest_candles.py
    |-- export_binance_klines.py
    |-- train_workflow.py
    `-- predict_latest.py
```

Entry point sketch:

```cpp
if (cfg.qlibOrchestration.enabled) {
    auto stateStore = std::make_shared<QlibStateStore>(cfg.qlibOrchestration);
    stateStore->initializeSchema();
    stateStore->initializeRuntimeStateIfMissing();
    stateStore->startReloadLoop(ctx.ioc());

    auto& pm = ProcessManager::instance();
    m_batchScheduler = std::make_unique<BatchSchedulerThread>(cfg.qlibOrchestration, pm, *stateStore);
    m_candleScheduler = std::make_unique<CandleSchedulerThread>(cfg.qlibOrchestration, pm, *stateStore);

    scanner.setOnKlineClosed([&](auto symbol, auto interval, auto openMs, auto closeMs) {
        m_candleScheduler->notifyCandleClose(symbol, interval, openMs, closeMs);
    });
}
```

SignalEngine must receive `QlibStateStore` or a narrower `IExecutionStatePort` so it can decide shadow/live behavior without parsing `config.json` again.

---

## 13. Done-When Criteria

| Criterion | Verification |
|-----------|--------------|
| Static qlib strategy is configured at startup | `strategies[]` contains `qlib_model_signal` before bot start |
| No runtime config injection is used | No code path writes `strategies[]` for qlib activation |
| State initializes to shadow | `qlib_runtime_state.execution_mode='shadow'` on first run |
| Batch Phase 2 command includes required args | Logs include `--horizon-bars` and `--report-out` |
| Model publish is atomic | Active manifest changes only after model and report validate |
| Active model is never overwritten in place | Model artifacts are versioned by `run_id` |
| Phase 3 refreshes latest candle before predict | `qlib_candles` and dataset contain `(symbol, interval, asof)` before prediction |
| `predict_latest.py` writes `run_id` and `horizon_bars` | `qlib_predictions` has non-null values for new predictions |
| Shadow path records would-trade gates | `qlib_shadow_signals` has `blocked_stage` and `would_place_order` rows |
| Shadow outcomes mature only after horizon | `qlib_shadow_outcomes` appears only after exit candle exists |
| Promotion waits at least 2 weeks | No transition out of shadow before `promotionMinShadowDays` |
| Promotion uses net returns and drift checks | `qlib_promotion_evaluations` records hit, Sharpe, mean net return, drift, stale ratio |
| Live canary is limited | Orders in canary use `canaryRiskMultiplier` |
| Rollback is automatic | Breach writes `execution_mode='shadow'` and rollback reason |
| State reload works without config reload | Changing SQLite execution mode changes behavior without restarting |

---

## 14. Risk Register

| Risk | Severity | Mitigation |
|------|----------|------------|
| Phase 3 predicts against stale dataset | High | LatestCandleRefresher must upsert the asof candle before prediction. |
| Shadow metrics diverge from live gates | High | Record metrics inside SignalEngine after the same gates used for live placement. |
| Promotion overfits a short dry-run window | High | Require 14 days, mature outcomes, net-return checks, drift checks, and canary. |
| Model file partially read during publish | High | Versioned immutable artifacts plus atomic active manifest. |
| Config reload assumption is false | High | Mutable state is in SQLite; static strategy exists from startup. |
| SQLite lock contention during scans | Medium | WAL mode, busy_timeout, short transactions, no long writes in evaluate path. |
| Latest candle refresh misses Binance finalization | Medium | Delay after close, retry, and skip prediction if required symbols are missing. |
| Live canary loses money | Medium | Reduced risk multiplier and automatic rollback. |
| Bot restart leaves stale running job rows | Low | Job ledger stale detection and Job Object process cleanup. |
| Drift check rejects valid regime shift | Medium | Store evaluation details; tune thresholds after shadow observation. |

---

## 15. Review Finding Closure

| Finding | Fix in v1.1 |
|---------|-------------|
| Phase 3 had no fresh candle data | Added LatestCandleRefresher before prediction and `qlib_candles` store. |
| Design assumed config hot reload | Removed runtime strategy injection; added SQLite runtime state and QlibStateStore reload. |
| Dry-run returned `Direction::None`, preventing shadow metrics | Added SignalEngine shadow execution path and `qlib_shadow_signals`/`qlib_shadow_outcomes`. |
| Auto-promotion was too weak | Added 14-day minimum, mature signal count, drift, net-return, canary, and rollback gates. |
| PromotionChecker had no actual returns | Added `qlib_actual_returns` and ActualReturnUpdater keyed by `horizon_bars`. |
| Phase 2 command missed required args | Added `--horizon-bars` and `--report-out` to the Phase 2 command. |
| Model publish could race with prediction | Added ModelPublisher with versioned artifacts and atomic manifest replacement. |
| ProcessManager mutex could serialize Phase 2 and Phase 3 | Specified locks must not be held while waiting for subprocess completion. |
| One pending candle field could drop/coalesce work incorrectly | Replaced `m_pendingCandleMs` with a deduped queue keyed by `(interval, asof_open_time_ms)`. |
| Restart/orphan handling was underspecified | Added job ledger, stale job detection, and Windows Job Object cleanup. |
