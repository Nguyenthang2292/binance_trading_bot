# Comprehensive Review: `src/orchestration` Module

**Date:** 2026-05-21
**Reviewer:** Claude Code (manual multi-dimensional review — quality, architecture, security, performance, concurrency, testing, docs)
**Scope:** `src/orchestration/` — all 16 files (5 headers, 5 implementations, 6 header-only helpers / configs / ports). Cross-referenced with `tests/test_process_manager.cpp`, `tests/test_batch_scheduler_thread.cpp`, `tests/test_candle_scheduler_thread.cpp`, `tests/test_promotion_checker.cpp`, `src/main.cpp` (wiring), and `config.json`.
**Module:** Qlib Orchestration runtime (`ProcessManager`, `BatchSchedulerThread`, `CandleSchedulerThread`, `QlibStateStore`, `PromotionChecker`, `ShadowMetricsRecorder`, `ModelPublisher`)
**Prior reviews:** none for this module (new code per `docs/design/2026-05-20-qlib-orchestration-v1.1.md`).

---

## Overview

The orchestration module drives the 4-phase Qlib model lifecycle: nightly batch export+train (Phase 1+2), per-candle prediction (Phase 3), and shadow→canary→live promotion (Phase 4). It runs as two long-lived `std::jthread`s (`batch`, `candle`) plus a Boost.Asio steady_timer that refreshes the SQLite-backed runtime state every N seconds. The signal engine consults `IExecutionStatePort::snapshot()` per candidate signal and records shadow metrics via `IShadowMetricsPort`.

The module is structurally clean: each phase has a single responsibility, dependencies flow inward through `IProcessRunner` / `IExecutionStatePort` / `IShadowMetricsPort` interfaces, and SQL is fully parameterized. Tests cover the happy paths for ProcessManager (5), batch scheduling (5), candle scheduling (5), and promotion (7). ModelPublisher and ShadowMetricsRecorder, however, have **zero direct unit tests** — they are only exercised transitively through batch and candle tests.

The biggest concerns are:

1. **POSIX ProcessManager has no timeout** — `std::system()` blocks indefinitely on Linux; only Windows enforces `timeoutSeconds`. A hung Python child silently freezes the batch/candle threads.
2. **Timer reset race in `QlibStateStore::stopReloadLoop`** — `m_reloadTimer.reset()` runs immediately after `cancel()` without waiting for the in-flight async_wait to complete, which is unsafe on a multi-threaded io_context (2 threads per `main.cpp`).
3. **`ModelPublisher::publish` is not transactionally consistent across filesystem + SQLite** — directory move and manifest swap happen *before* `BEGIN IMMEDIATE`, so a crash between them leaves the artifact tree updated but `qlib_runtime_state.active_run_id` stale (and the row check at line 335 only detects truly missing rows, not the half-applied state).
4. **`ShadowMetricsRecorder::onCandleClosed` does an O(rows) join over `qlib_candles × qlib_shadow_signals` on every candle close** — fine at smoke-test scale (<1k candles), but unindexed and will degrade once production has weeks of history.

| Dimension | Assessment |
|---|---|
| Code quality | Good — small functions, clear naming, RAII for SQLite/OpenSSL handles. |
| Architecture | Good layering. Port interfaces let engine stay test-friendly. Phase 4 polling design is right for cron-style work. |
| Security / data integrity | Mixed. Parameterized SQL, sha256 for artifacts. But unverified env-var injection in `buildPhase1Cmd` and POSIX shell invocation in `ProcessManager`. |
| Performance | Adequate at current scale. Shadow outcome join is a future hot spot. |
| Concurrency | One real race (`stopReloadLoop`), one shaky pattern (timer reset). Locking is otherwise correct. |
| Testing | 22 tests across 4 files; ModelPublisher and ShadowMetricsRecorder lack direct coverage. |

---

## Critical (3)

### CR-1: POSIX `ProcessManager::spawnOnce` has no timeout enforcement — hung child freezes the orchestrator

**Files:** [`process_manager.cpp:251-269`](../../src/orchestration/process_manager.cpp), [`process_manager.h:19`](../../src/orchestration/process_manager.h)

The Windows path implements timeout correctly via `WaitForSingleObject(pi.hProcess, waitMs)` with `TerminateProcess` on timeout ([`process_manager.cpp:231-237`](../../src/orchestration/process_manager.cpp)). The POSIX path is:

```cpp
const int rc = std::system(shell.str().c_str());
if (WIFEXITED(rc)) {
    out.exitCode = WEXITSTATUS(rc);
} else {
    out.exitCode = rc;
}
out.timedOut = false;   // <-- always false on POSIX, even if hung for hours
out.succeeded = out.exitCode == 0;
```

`std::system` blocks until the shell returns. There is no `alarm()`, no `popen` + select loop, no SIGKILL on deadline. The configured `timeoutSeconds` is silently ignored on Linux/macOS.

Consequence: if `train_workflow.py` (Phase 2, default 300s timeout) hangs on a CUDA initialization stall or HTTP fetch, the `BatchSchedulerThread::run()` loop is blocked in `spawnWithRetry`. The thread holds `m_mutex` for the whole `spawnWithRetry` ([`process_manager.cpp:135`](../../src/orchestration/process_manager.cpp)) — but no other path takes that mutex, so the bigger concern is the thread itself: it can never advance to the next batch day, never prune logs, never call `pruneOldLogs`. `CandleSchedulerThread` runs independently so per-candle predictions still work — but a single stuck call still wedges the daily training pipeline indefinitely.

The Windows test `ProcessManagerTest.TimeoutKillsProcess` ([`tests/test_process_manager.cpp:106-120`](../../tests/test_process_manager.cpp)) only runs `ping`, which exists on Windows; on POSIX the same test runs `/bin/sh -c "sleep 30"` and *expects `timedOut=true`* — that assertion will fail on Linux, because the POSIX implementation reports `timedOut=false` and waits the full 30 seconds. The test is currently a false negative for the Linux build.

**Fix:** use `posix_spawn` + `waitpid(WNOHANG)` polling against a deadline, or `fork` + `execvp` + parent-side timeout with `kill(SIGKILL)`. The `gemini_filter.cpp` POSIX path ([`gemini_filter.cpp:613-712`](../../src/engine/gemini_filter.cpp)) already does this correctly — that pattern can be lifted into `ProcessManager`.

---

### CR-2: `QlibStateStore::stopReloadLoop` resets the timer while async_wait may still be pending — UB on multi-threaded io_context

**Files:** [`qlib_state_store.cpp:136-144`](../../src/orchestration/qlib_state_store.cpp), [`qlib_state_store.cpp:194-208`](../../src/orchestration/qlib_state_store.cpp), [`main.cpp:723-724`](../../src/main.cpp)

```cpp
void QlibStateStore::stopReloadLoop() {
    m_reloadRunning.store(false);
    if (!m_reloadTimer) {
        return;
    }
    boost::system::error_code ec;
    m_reloadTimer->cancel(ec);
    m_reloadTimer.reset();          // <-- destroys timer while pending handler may still be in flight
}
```

`cancel()` does not synchronously complete pending wait operations — it schedules them to run with `ec=operation_aborted`. On a multi-threaded io_context (`contextConfig.threadPoolSize = 2` in [`main.cpp:724`](../../src/main.cpp)), the in-flight handler can be mid-execution on thread B while thread A executes `cancel()` then `reset()`. Destroying the `steady_timer` while boost::asio still references its internal state during handler dispatch is undefined behavior.

The class is correctly designed with `enable_shared_from_this` and `weak_from_this()` ([`qlib_state_store.h:31`](../../src/orchestration/qlib_state_store.h), [`qlib_state_store.cpp:199`](../../src/orchestration/qlib_state_store.cpp)) — that keeps the *object* alive across the handler. The bug is the *timer subobject* being destroyed too eagerly.

Currently `stopReloadLoop` is only called from `~QlibStateStore` and main's shutdown path, both of which run after the engine stops generating new work — so in practice the race window is small and crashes are unlikely. But it remains genuinely incorrect under concurrent shutdown.

**Fix:** drop `m_reloadTimer.reset()`. The unique_ptr will release when the object is destroyed. Or, before resetting, post a "drain" handler to the timer's executor that signals completion (boost::asio idiom).

```cpp
void QlibStateStore::stopReloadLoop() {
    m_reloadRunning.store(false);
    if (m_reloadTimer) {
        boost::system::error_code ec;
        m_reloadTimer->cancel(ec);
        // Do not reset; let destruction occur with the owning object.
    }
}
```

The `m_reloadRunning=false` + `if (!self->m_reloadRunning.load()) return` check at [`qlib_state_store.cpp:202`](../../src/orchestration/qlib_state_store.cpp) is sufficient to prevent further `scheduleReload` calls.

---

### CR-3: `ModelPublisher::publish` filesystem changes are not rolled back if the SQLite transaction fails

**Files:** [`model_publisher.cpp:159-351`](../../src/orchestration/model_publisher.cpp)

The publish sequence is:

1. Validate staging dir has `model.txt` and `report.json` (lines 165–172).
2. Open DB, set PRAGMAs, preflight `qlib_runtime_state` row exists (lines 174–195).
3. **Move staging → artifacts** (`moveRunDirectory`, lines 197–205). This is irreversible — the staging dir is gone.
4. Hash files, build manifest JSON (lines 220–237).
5. **Atomic write the manifest** via `tmp + MoveFileExW` / `rename` (lines 240–261). Now the model is "published" on disk.
6. `BEGIN IMMEDIATE TRANSACTION` (line 263). Upsert `qlib_model_runs` + update `qlib_runtime_state`. If either fails, `ROLLBACK`.

If the transaction at step 6 fails (disk full, DB locked beyond busy_timeout, schema mismatch) we **rollback the DB** but leave steps 3–5 done: the artifacts directory holds the new run, and `current/<modelId>.json` already points at it. Next process start will re-read `qlib_runtime_state` (still pointing at the *previous* `active_run_id`) and serve predictions from a stale model, while the manifest on disk says otherwise.

The same gap exists in reverse: if rename of manifest fails (step 5), we've moved the staging dir but never updated the runtime state — `runStaging` is gone, so a retry will fail at step 1 ("missing model.txt in staging run dir"). Publish is now permanently stuck for this `run_id`.

This is a real durability issue for a system that promises atomic model promotion.

**Suggested fix:** invert the order so DB updates land first, then filesystem mutations. Stage manifest at `tmpPath`, take the DB lock with `BEGIN IMMEDIATE`, upsert all rows, then move dir + rename manifest, then commit. On rollback after dir move, attempt a best-effort `rename(artifactRunDir, runStaging)` to restore staging. Alternatively, accept that filesystem is the source of truth and treat the DB row as a derived index — but then `qlib_runtime_state.active_run_id` should be reloaded from the manifest on startup, which it currently isn't.

A simpler interim fix: copy (don't move) staging to artifacts, then commit DB, then `remove_all(runStaging)`. Costs ~100MB of disk during the transition window but is safely reversible.

---

## Warning (5)

### WR-1: `BatchSchedulerThread::buildPhase1Cmd` reads `QLIB_DUMP_BIN_PATH` from environment without validation

**Files:** [`batch_scheduler_thread.cpp:100-110`](../../src/orchestration/batch_scheduler_thread.cpp)

```cpp
if (const char* dumpBin = std::getenv("QLIB_DUMP_BIN_PATH"); dumpBin && *dumpBin) {
    cmd.push_back("--convert-mode");
    cmd.push_back("incremental");
    cmd.push_back("--dump-bin-script");
    cmd.push_back(std::string(dumpBin));
    ...
}
```

The value is passed verbatim to the Python subprocess as the `--dump-bin-script` argument. The Python side (per the design doc, [`docs/design/2026-05-20-qlib-orchestration-v1.1.md`](../../docs/design/2026-05-20-qlib-orchestration-v1.1.md)) typically calls `subprocess.run([sys.executable, dump_bin_script, ...])`. If the env var points at a malicious script, that script runs with the bot's full privileges.

In a single-user development environment this is academic — the operator controls the env. But on a shared host or CI runner, anyone able to set environment variables (e.g. via Docker/Kubernetes manifest, systemd unit override) can trivially run arbitrary code at the next batch tick. There is no logged audit trail of the resolved path either.

**Fix:** validate that the env value resolves to a file under a known prefix (`tools/qlib_bridge/`, `scripts/`), reject otherwise. Log the resolved path with the cycle's run_id so operators can audit. Or, drop the env var entirely and put the path into `config.json` under `qlib_orchestration.dump_bin_script`.

### WR-2: `ShadowMetricsRecorder::upsertActualReturnsLocked` lacks an index on `qlib_actual_returns`

**Files:** [`shadow_metrics_recorder.cpp:386-452`](../../src/orchestration/shadow_metrics_recorder.cpp), [`shadow_metrics_recorder.cpp:61-94`](../../src/orchestration/shadow_metrics_recorder.cpp)

The maturation query LEFT JOINs `qlib_actual_returns` to find rows that still need to be computed:

```sql
LEFT JOIN qlib_actual_returns ar
  ON ar.symbol = e.symbol
 AND ar.interval = e.interval
 AND ar.asof_open_time_ms = e.open_time_ms
 AND ar.horizon_bars = ?
WHERE e.interval = ? AND ar.symbol IS NULL;
```

`qlib_actual_returns` has PK `(symbol, interval, asof_open_time_ms, horizon_bars)` — that index covers the JOIN. But the join *also* requires scanning all rows of `qlib_candles e` for the interval, joined to the *next-bar* candle `x`, every time a single candle closes. There is no index on `qlib_candles(interval, open_time_ms)`; the PK is `(symbol, interval, open_time_ms)`, which doesn't help when the leading column isn't constrained.

Smoke-test scale (10 symbols × 168 candles per week ≈ 1.7k rows) runs in milliseconds. A year of data at 1h interval × 100 symbols ≈ 876k candle rows; the LEFT JOIN scan grows linearly. SQLite will do a full table scan on the outer `e` and may slow each candle close to seconds.

**Fix:** add `CREATE INDEX IF NOT EXISTS idx_qlib_candles_interval_open ON qlib_candles(interval, open_time_ms)` to `initializeSchema()`. Same for `idx_qlib_actual_returns_interval_asof` if you anticipate ad-hoc analytics.

### WR-3: `ShadowMetricsRecorder::recordShadowSignal` uses `SQLITE_TRANSIENT` for every bind but never resets the statement for reuse — fresh prepare per call

**Files:** [`shadow_metrics_recorder.cpp:175-293`](../../src/orchestration/shadow_metrics_recorder.cpp)

Every `recordShadowSignal` call prepares two statements (`SELECT prediction` + `INSERT shadow_signals`), binds, executes, finalizes. At 100 symbols × hourly signals that's ~4800 prepares/day — small in absolute terms, but the `qlib_predictions` lookup is on the hot signal path inside `processItem`. The pattern is OK for batch jobs but unnecessary work for steady-state hot paths.

Compare to `RiskController` (separate module) which caches prepared statements. Suggest a `sqlite3_stmt*` member cached behind the mutex, reset+rebind on reuse. Low priority unless profiling shows it.

### WR-4: `PromotionChecker::barsPerYear` silently returns the hourly default for unknown intervals

**Files:** [`promotion_checker.cpp:132-148`](../../src/orchestration/promotion_checker.cpp)

```cpp
double PromotionChecker::barsPerYear(const std::string& interval) {
    if (interval.size() < 2) {
        return 365.0 * 24.0;
    }
    const char suffix = interval.back();
    const int value = std::max(1, std::atoi(interval.substr(0, interval.size() - 1).c_str()));
    if (suffix == 'm') { ... }
    if (suffix == 'h') { ... }
    if (suffix == 'd') { ... }
    return 365.0 * 24.0;   // fallback for unknown suffixes ('w', 'M', etc.)
}
```

A misconfigured `interval="1w"` would compute an annualized Sharpe as if bars were hourly — overstating Sharpe by ~168×. That promotes Shadow→Canary→Live on essentially fictional statistics.

**Fix:** return `std::nan` (or `std::nullopt` via signature change) for unrecognized intervals; the caller should refuse to promote when annualization is impossible. Log a startup-time warning when the configured interval isn't one of {m, h, d}.

The companion function `ShadowMetricsRecorder::intervalToMs` ([`shadow_metrics_recorder.cpp:328-347`](../../src/orchestration/shadow_metrics_recorder.cpp)) returns 0 for unknown intervals and the caller checks `if (stepMs <= 0) return;` — that's the safer pattern.

### WR-5: `BatchSchedulerThread::run` busy-polls with 1-second sleeps instead of scheduling to the next batch window

**Files:** [`batch_scheduler_thread.cpp:217-228`](../../src/orchestration/batch_scheduler_thread.cpp)

```cpp
while (!stopToken.stop_requested()) {
    (void)runScheduledCycleAt(std::chrono::system_clock::now());

    const auto nowSys = std::chrono::system_clock::now();
    const auto nowSec = std::chrono::time_point_cast<std::chrono::seconds>(nowSys);
    const auto nextMinute = nowSec + std::chrono::minutes(1);
    while (!stopToken.stop_requested() && std::chrono::system_clock::now() < nextMinute) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
```

The thread spins 60 times per minute, 86,400 times per day, just to check "is it time to run yet?" Most of those checks return immediately because `m_lastRunDate == today` from `runScheduledCycleAt`. Functional but wasteful — and the 1s granularity means up to 1s of latency at the `batchHour:batchMinute` boundary, with no benefit.

A `std::condition_variable_any` + `wait_for(stopToken, untilNextBatchTime)` would be cancellable, precise, and consume zero CPU between cycles.

Low priority — affects one thread, one second per second.

---

## Info / Style (4)

### IN-1: `QlibStateStore::reloadStateOnce` swallows exception but the fallback snapshot is misleading

**Files:** [`qlib_state_store.cpp:177-192`](../../src/orchestration/qlib_state_store.cpp)

On query failure (`db locked > 5s`, `disk I/O error`), the snapshot is reset to `{available=false, mode=Shadow}`. `available=false` is correct, but `mode=Shadow` is then read by `SignalEngine` and treated as "Shadow execution" — which suppresses live ordering (good fail-safe) but also triggers shadow-recording logic ([`signal_engine.cpp:550`](../../src/engine/signal_engine.cpp)) on a snapshot that's known to be stale. Net effect: shadow metrics are recorded against a model_id+run_id pair that may no longer be active.

Setting `mode=Disabled` for the unavailable case would suppress *all* qlib-strategy work, which is the cleanest fail-safe. Document the intent either way.

### IN-2: `ModelPublisher::sha256File` does not handle read errors mid-file consistently

**Files:** [`model_publisher.cpp:68-99`](../../src/orchestration/model_publisher.cpp)

```cpp
while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize bytesRead = input.gcount();
    ...
}
if (!input.eof()) {
    throw std::runtime_error("failed while reading file for sha256: " + path.string());
}
```

If the read sets `failbit | eofbit` together with the last partial read, `gcount()` returns the partial bytes and they are correctly digested. But the post-loop check `if (!input.eof())` then succeeds (eof is set), so a partial-read-and-failure is indistinguishable from clean EOF. SHA256 will silently be computed over the prefix of the file. Low-probability scenario (transient I/O error during model.txt read), but the hash would then be wrong and stored as the dataset fingerprint.

Cleaner: check `if (input.bad() || (!input.eof() && input.fail()))` to distinguish hard I/O errors from clean EOF.

### IN-3: `CandleSchedulerThread::run` uses `std::max` directly without `<algorithm>` include

**Files:** [`candle_scheduler_thread.cpp`](../../src/orchestration/candle_scheduler_thread.cpp), line 21 (`std::max`) and line 101 (`std::max`)

Compiles today because `<algorithm>` is transitively pulled in via standard library headers, but the source only includes `<chrono>`, `<filesystem>`, `<thread>`. Should add `#include <algorithm>` explicitly. Cosmetic but breaks on library updates.

### IN-4: `ModelPublisher::ensureRuntimeStateExists` returns a misleading error suggesting initialization order

**Files:** [`model_publisher.cpp:101-126`](../../src/orchestration/model_publisher.cpp)

```cpp
if (rc == SQLITE_DONE) {
    error = "qlib_runtime_state row not found for model_id/interval; call initializeRuntimeStateIfMissing first";
    return false;
}
```

The runtime state row is only ever created by `QlibStateStore::initializeRuntimeStateIfMissing()` ([`qlib_state_store.cpp:106-125`](../../src/orchestration/qlib_state_store.cpp)). In `main.cpp` this is called once at startup ([`main.cpp:927`](../../src/main.cpp)). The error message implies a developer-reachable contract violation — but in practice this can only happen if the operator deletes the row from the DB, or if model_id/interval mismatches between `QlibStateStoreConfig` and `ModelPublishRequest`. The actionable error here is "model_id/interval mismatch between state store and publish request" — note the exact pair.

---

## What looks good

- **Port abstraction is genuine.** `IProcessRunner`, `IExecutionStatePort`, `IShadowMetricsPort` are used in tests (`FakeProcessRunner`) and let `SignalEngine` ignore the orchestration layer entirely when disabled. Clean dependency inversion.
- **All SQL is parameterized.** `bindText`/`bindInt64` everywhere; no string concatenation into SQL. Zero injection surface.
- **RAII for sqlite3_stmt and sqlite3 handles.** `unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>` is used consistently — exception-safe finalization. Same for `sqlite3*` in `PromotionChecker` and `ModelPublisher`.
- **OpenSSL handle ownership.** `EVP_MD_CTX_new()` paired with `unique_ptr<EVP_MD_CTX, EVP_MD_CTX_free>` ([`model_publisher.cpp:55-77`](../../src/orchestration/model_publisher.cpp)). Correct.
- **Atomic manifest swap on Windows via `MoveFileExW(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)`.** Stronger guarantee than the POSIX `rename` fallback, and explicitly chosen.
- **`enable_shared_from_this` with `[[nodiscard]] create()` factory and clear header comment.** [`qlib_state_store.h:27-35`](../../src/orchestration/qlib_state_store.h) — explicit and prevents footguns.
- **Schema migration via `PRAGMA table_info` + conditional `ALTER TABLE`.** [`shadow_metrics_recorder.cpp:152-172`](../../src/orchestration/shadow_metrics_recorder.cpp) — survives multiple version upgrades cleanly.
- **`PromotionChecker::evaluate` is a pure state machine.** Shadow→Canary→Live transitions are gated only by Sharpe + hit-rate + sample size. Easy to test, easy to audit.
- **`BatchSchedulerThread` is idempotent per-day.** `m_lastRunDate == today` gate prevents double-runs even after restart within the same day (modulo persistent state, see suggestion).

---

## Suggestions for next iteration

1. **Add direct unit tests for `ModelPublisher` and `ShadowMetricsRecorder`.** Both have non-trivial business logic (filesystem ↔ DB ordering, cost model, maturation join). The batch / candle tests are integration-level and don't isolate failures.
2. **Persist `BatchSchedulerThread::m_lastRunDate`** to disk or to `qlib_job_runs`. Right now a process restart at 08:00 on a batch day will re-run the batch even if it finished at 07:30. The `qlib_job_runs` table already exists ([`qlib_state_store.cpp:90-103`](../../src/orchestration/qlib_state_store.cpp)) but is never written from `BatchSchedulerThread::runOnce`.
3. **Consider deduplicating `intervalToMs` between `ShadowMetricsRecorder` and `PromotionChecker::barsPerYear`.** Both parse the same string format with subtly different error semantics. A single `IntervalSpec::parse(...) -> std::optional<IntervalSpec>` in a shared header would eliminate the divergence flagged in WR-4.
4. **Log the resolved Phase 1/2/3 command lines at INFO once per cycle** (currently only logged on failure via `phase{1,2,3}.logPath`). Helps operators correlate orchestration logs with Python-side runs.

---

## Severity Summary

| ID | Title | Severity |
|---|---|---|
| CR-1 | POSIX `ProcessManager` ignores `timeoutSeconds` | 🔴 Critical |
| CR-2 | `QlibStateStore::stopReloadLoop` timer reset race | 🔴 Critical |
| CR-3 | `ModelPublisher::publish` non-atomic across FS+DB | 🔴 Critical |
| WR-1 | `QLIB_DUMP_BIN_PATH` env var passed unchecked | 🟡 Warning |
| WR-2 | Missing index on `qlib_candles(interval, open_time_ms)` | 🟡 Warning |
| WR-3 | Prepare-per-call on hot shadow signal path | 🟡 Warning |
| WR-4 | `PromotionChecker::barsPerYear` silent fallback | 🟡 Warning |
| WR-5 | Batch scheduler busy-polls every second | 🟡 Warning |
| IN-1 | Fallback snapshot reports `Shadow` instead of `Disabled` | ⚪ Info |
| IN-2 | `sha256File` doesn't distinguish partial-read from EOF | ⚪ Info |
| IN-3 | Missing `<algorithm>` include in candle scheduler | ⚪ Info |
| IN-4 | Misleading error message in `ensureRuntimeStateExists` | ⚪ Info |

**Verdict:** **Request Changes.** The three critical findings are real correctness/durability issues. CR-1 makes the Linux build silently non-functional with hanging Python subprocesses; CR-2 is latent UB under shutdown; CR-3 is a real data-integrity gap if SQLite ever fails mid-publish. The warnings and info items can be addressed in a follow-up pass.
