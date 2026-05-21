# Comprehensive Review: `src/orchestration` Module (Final)

**Date:** 2026-05-21
**Reviewer:** Claude Code (manual multi-dimensional review)
**Scope:** `src/orchestration/` — confirming the v2.0 "Approve" verdict with no source changes since that review.
**Module:** Qlib Orchestration runtime
**Prior review:** [`2026-05-21-orchestration-v2.0.md`](2026-05-21-orchestration-v2.0.md) — all 12 original findings + NW-1 resolved.

---

## Overview

No orchestration source files were modified since the v2.0 review. This pass re-reads the key paths to confirm no regression was introduced via adjacent changes (config.json restructure, engine risk-profile wiring, CMakeLists updates).

All items from v2.0 remain resolved. No new findings.

---

## Spot-Check Verification

| Area | Finding | Status |
|---|---|---|
| `shadow_metrics_recorder.cpp:358-401` | WR-3 cached statements — `predictionLookupStmtLocked()` and `insertShadowSignalStmtLocked()` lazy-init once, `finalizeStatements()` cleans up | ✅ Intact |
| `process_manager.cpp` POSIX path | CR-1 fork/waitpid/SIGKILL loop with `steady_clock` deadline | ✅ Intact |
| `qlib_state_store.cpp:136-143` | CR-2 timer reset removed; `m_reloadRunning` gates re-schedule | ✅ Intact |
| `model_publisher.cpp:266-437` | CR-3 `BEGIN IMMEDIATE` + `rollbackDb()` + `rollbackFilesystem()` on every failure path | ✅ Intact |
| `batch_scheduler_thread.cpp:297-345` | WR-5 `nextWakeTime()` + `wait_until` + `stop_callback` | ✅ Intact |
| `promotion_checker.cpp:143-164` | WR-4 `barsPerYear()` returns `std::nan` for unknown intervals; `sharpe=NaN` blocks promotion | ✅ Intact |
| `qlib_state_store.cpp:183` | IN-1 fallback `mode = ExecutionMode::Disabled` | ✅ Intact |

---

## New Test Coverage (committed in this pass)

- **`tests/test_batch_scheduler_thread.cpp`** (232 lines) — schedule calculation, `nextWakeTime` across weekends and 14-day horizon, `stop_callback` shutdown.
- **`tests/test_candle_scheduler_thread.cpp`** (306 lines) — candle-close notification routing, deduplication, shutdown path.
- **`tests/test_promotion_checker.cpp`** (217 lines) — Sharpe/UPI promotion gates, NaN blocking, per-interval `barsPerYear` coverage.
- **`tests/test_qlib_model_signal_plugin.cpp`** (406 lines) — plugin signal generation against live and shadow execution modes.

These extend the orchestration test surface beyond the six files noted in v2.0.

---

## Outstanding Items

None. All findings from v1.0 and v2.0 are resolved.

---

## Verdict

**Approve.** The orchestration module verdict is unchanged from v2.0. No regressions, no new findings.

---

## Severity Summary

| ID | Title | v1.0 | v2.0 | v3.0 |
|---|---|---|---|---|
| CR-1 | POSIX `ProcessManager` timeout | 🔴 Critical | ✅ Resolved | ✅ Unchanged |
| CR-2 | `QlibStateStore` timer reset race | 🔴 Critical | ✅ Resolved | ✅ Unchanged |
| CR-3 | `ModelPublisher` non-atomic publish | 🔴 Critical | ✅ Resolved | ✅ Unchanged |
| WR-1 | env-var path injection | 🟡 Warning | ✅ Resolved | ✅ Unchanged |
| WR-2 | missing candle index | 🟡 Warning | ✅ Resolved | ✅ Unchanged |
| WR-3 | prepare-per-call on hot path | 🟡 Warning | ✅ Resolved | ✅ Unchanged |
| WR-4 | `barsPerYear` silent fallback | 🟡 Warning | ✅ Resolved | ✅ Unchanged |
| WR-5 | batch scheduler busy-poll | 🟡 Warning | ✅ Resolved | ✅ Unchanged |
| IN-1 | Shadow vs Disabled fallback | ⚪ Info | ✅ Resolved | ✅ Unchanged |
| IN-2 | `sha256File` partial-read | ⚪ Info | ✅ Resolved | ✅ Unchanged |
| IN-3 | missing `<algorithm>` | ⚪ Info | ✅ Resolved | ✅ Unchanged |
| IN-4 | error message wording | ⚪ Info | ✅ Improved | ✅ Unchanged |
| NW-1 | execvp 127 vs child 127 ambiguity | — | ✅ Resolved | ✅ Unchanged |
