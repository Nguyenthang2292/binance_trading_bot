# Comprehensive Review: `src/orchestration` Module (Follow-up)

**Date:** 2026-05-21
**Reviewer:** Claude Code (manual multi-dimensional review — quality, architecture, security, performance, concurrency, testing, docs)
**Scope:** `src/orchestration/` — verifying fixes from [`2026-05-21-orchestration-v1.0.md`](2026-05-21-orchestration-v1.0.md). Cross-referenced with new tests (`tests/test_model_publisher.cpp`, `tests/test_shadow_metrics_recorder.cpp`), updated `process_manager.cpp`, `qlib_state_store.cpp`, `model_publisher.cpp`, `batch_scheduler_thread.cpp`, `shadow_metrics_recorder.cpp`, `promotion_checker.cpp`.
**Module:** Qlib Orchestration runtime
**Prior review:** [`2026-05-21-orchestration-v1.0.md`](2026-05-21-orchestration-v1.0.md) — 3 critical, 5 warning, 4 info; all 12 verified below.

---

## Overview

This is a **follow-up** review. The first pass raised 12 findings; **all 3 critical, all 5 warning, and all 4 info findings are now resolved or improved**. Two new direct unit-test files (`test_model_publisher.cpp`, `test_shadow_metrics_recorder.cpp`) close the testing gap flagged in v1.0. The follow-up `ProcessManager` POSIX diagnostic item raised in this pass is also fixed and covered by a regression test.

The module is materially safer now: the POSIX ProcessManager enforces timeouts, the QlibStateStore shutdown is race-free, ModelPublisher rolls back both filesystem and DB on failure, and the batch scheduler no longer wastes CPU polling 86,400 times per day.

| Dimension | v1.0 → v2.0 |
|---|---|
| Code quality | Good → Good. Small refactors land cleanly. |
| Architecture | Good → Good. No structural change; only correctness fills. |
| Security / data integrity | Mixed → Good. Env-var allowlist + canonical path; filesystem+DB co-rollback. |
| Performance | Adequate → Better. Indexes added; busy-poll replaced with `wait_until`. |
| Concurrency | Two shaky patterns → None known. Timer reset removed, POSIX fork path is sane. |
| Testing | 22 tests / 4 files → 29 Windows tests / 30 POSIX tests across 6 orchestration-focused files. |

---

## Prior Review Verification

| ID | Issue | Verification |
|---|---|---|
| CR-1 | POSIX `ProcessManager` ignores `timeoutSeconds` | ✅ Fixed — [`process_manager.cpp:269-348`](../../src/orchestration/process_manager.cpp) now uses `fork`/`execvp` + `waitpid(WNOHANG)` polling against a `steady_clock` deadline, with `SIGKILL` on expiry and `exitCode=124`. The Windows path also gained `JobObject`-based group kill for child trees ([`process_manager.cpp:242-249`](../../src/orchestration/process_manager.cpp)). |
| CR-2 | `QlibStateStore::stopReloadLoop` resets the timer mid-flight | ✅ Fixed — [`qlib_state_store.cpp:136-143`](../../src/orchestration/qlib_state_store.cpp) drops the `m_reloadTimer.reset()` call and only invokes `cancel(ec)`. The unique_ptr will release naturally when the object is destroyed. `m_reloadRunning.store(false)` still gates re-scheduling in the handler. |
| CR-3 | `ModelPublisher::publish` non-atomic across FS+DB | ✅ Fixed — [`model_publisher.cpp:266-437`](../../src/orchestration/model_publisher.cpp) now orders: write tmp manifest → BEGIN IMMEDIATE → upsert run + state rows → create artifact dir → COPY (not move) staging → backup existing manifest → atomic manifest swap → COMMIT → best-effort cleanup. Any failure between BEGIN and COMMIT invokes `rollbackDb()` AND `rollbackFilesystem()` (the latter removes the artifact copy and restores the manifest from `.bak_publish`). Original staging is preserved until commit. |
| WR-1 | `QLIB_DUMP_BIN_PATH` passed unchecked | ✅ Fixed — [`batch_scheduler_thread.cpp:86-124`](../../src/orchestration/batch_scheduler_thread.cpp) introduces `validatedDumpBinScript()` that canonicalizes via `weakly_canonical`, asserts `is_regular_file`, and checks the path is contained within `{config.scriptsDir, cwd/tools/qlib_bridge, cwd/scripts}` using a tokenized `isPathWithin()` helper. Rejected values are logged at WARN and the cycle falls back to `--convert-mode none`. |
| WR-2 | Missing index on `qlib_candles(interval, open_time_ms)` | ✅ Fixed — [`shadow_metrics_recorder.cpp:98-105`](../../src/orchestration/shadow_metrics_recorder.cpp) creates both `idx_qlib_candles_interval_open` and `idx_qlib_actual_returns_interval_asof` at schema init. The new index covers the LEFT JOIN scan that was the future hot spot. |
| WR-3 | Prepare-per-call on hot signal path | ✅ Fixed — [`shadow_metrics_recorder.cpp:357-399`](../../src/orchestration/shadow_metrics_recorder.cpp) now caches the prediction lookup and shadow-signal insert prepared statements behind `predictionLookupStmtLocked()` / `insertShadowSignalStmtLocked()` and finalizes them in `finalizeStatements()`. `recordShadowSignal` resets and rebinds the cached statements instead of preparing/finalizing per call. |
| WR-4 | `PromotionChecker::barsPerYear` silent hourly fallback | ✅ Fixed — [`promotion_checker.cpp:143-164`](../../src/orchestration/promotion_checker.cpp) now returns `std::nan` for unrecognized suffixes/values via `std::from_chars` plus suffix dispatch, and the caller at lines 87–94 sets `out.sharpe = NaN` + logs a warning. Downstream `stats.sharpe >= minSharpe` is false for NaN, so promotion is correctly blocked. |
| WR-5 | Batch scheduler busy-polls every second | ✅ Fixed — [`batch_scheduler_thread.cpp:297-345`](../../src/orchestration/batch_scheduler_thread.cpp) introduces `nextWakeTime()` (walks up to 14 days for the next valid weekday + hour:minute pair) and `run()` now uses `std::condition_variable::wait_until` with a `std::stop_callback`. Zero polling, instant shutdown response. |
| IN-1 | Fallback snapshot reports Shadow instead of Disabled | ✅ Fixed — [`qlib_state_store.cpp:183`](../../src/orchestration/qlib_state_store.cpp) now sets `fallback.mode = ExecutionMode::Disabled` and the WARN log says "qlib execution disabled". Likewise `loadSnapshotLocked` (line 232) defaults to `Disabled` when the row is absent. Cleaner fail-safe. |
| IN-2 | `sha256File` partial-read vs EOF | ✅ Fixed — [`model_publisher.cpp:90`](../../src/orchestration/model_publisher.cpp) now distinguishes hard I/O failure with `if (input.bad() || (input.fail() && !input.eof()))`. EOF alone no longer masks a partial read. |
| IN-3 | Missing `<algorithm>` include | ✅ Fixed — [`shadow_metrics_recorder.cpp`](../../src/orchestration/shadow_metrics_recorder.cpp) now includes `<algorithm>` directly for `std::max`; `bot_lib` and orchestration tests compile cleanly. |
| IN-4 | Misleading error message in `ensureRuntimeStateExists` | ✅ Improved — the equivalent error string at [`model_publisher.cpp:366-367`](../../src/orchestration/model_publisher.cpp) now reads "qlib_runtime_state row not found for model_id='X' interval='Y'; check state-store and publish config use the same pair" — names the actual mismatched pair. |

---

## New Findings (1, Resolved)

### NW-1: `ProcessManager` POSIX fork path swallows `_exit(127)` exitcode collision with WIFEXITED→127

**Files:** [`process_manager.cpp:300-301`](../../src/orchestration/process_manager.cpp)

**Status:** ✅ Fixed.

The child still uses `_exit(127)` on `execvp` failure, preserving the shell convention "command not found", but the log now distinguishes a parent-side launch failure from a script that legitimately exits 127:

```cpp
execvp failed errno=<n> cmd=<cmd>
```

Implementation detail: [`process_manager.cpp`](../../src/orchestration/process_manager.cpp) prepares `argv` before `fork`, then the child path only performs descriptor setup, `execvp`, and a small `write(2)`-based diagnostic before `_exit(127)`. That avoids heap allocation in the post-fork child path while preserving the operator-facing diagnostic. `tests/test_process_manager.cpp` adds a POSIX-only regression test for this log line.

---

## Outstanding Items

None for the orchestration review scope. WR-3 is covered by cached prepared statements, IN-3 is covered by the explicit include, and NW-1 is covered by the POSIX exec-failure diagnostic plus regression test.

---

## New tests landed

- **`tests/test_model_publisher.cpp`** (6.5 KB) — direct coverage of publish happy path, rollback on DB failure, rollback on artifact-dir collision, manifest backup restore. Closes the v1.0 testing gap.
- **`tests/test_shadow_metrics_recorder.cpp`** (5.6 KB) — direct coverage of schema init, candle upsert, actual-return computation, shadow outcome maturation with cost model.
- **`tests/test_process_manager.cpp`** — POSIX-only coverage that an `execvp` launch failure writes an actionable diagnostic before returning exit code 127.

The first two were the gap explicitly called out in v1.0; they are now closed. The `ProcessManager` test covers the new v2.0 follow-up item.

---

## What looks good (now)

- **POSIX `ProcessManager` is cleanly written.** The fork → waitpid(WNOHANG) → deadline check → kill loop is the standard pattern (matches the existing `gemini_filter.cpp` POSIX path). 20ms sleep slice is reasonable. Log file is opened with `O_CREAT|O_WRONLY|O_TRUNC` in the parent then dup2'd in the child — order is correct (parent holds the fd until fork, child closes after dup2).
- **`replaceFileAtomically` is portable.** Windows uses `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`; POSIX uses `std::filesystem::rename`. Both atomic semantics.
- **`rollbackFilesystem` lambda captures by reference and stages cleanup state via the booleans `copiedArtifact`/`swappedManifest`/`hadPreviousManifest`.** Easy to read, easy to audit. Backup is `.bak_publish` (specific suffix) — no chance of colliding with user files.
- **`nextWakeTime` walks 14 days.** Covers any week-shaped schedule including week-ends + holidays + DST shifts. Returns `now + 1h` if 14 days yields nothing (safety fallback). Correct.
- **`PromotionChecker::barsPerYear` uses `std::from_chars`.** Properly rejects partial-number parses (e.g. `"1.5h"` → fails because `ptr != end`). The `from_chars` + suffix check is the right pattern.

---

## Severity Summary

| ID | Title | v1.0 | v2.0 |
|---|---|---|---|
| CR-1 | POSIX `ProcessManager` timeout | 🔴 Critical | ✅ Resolved |
| CR-2 | `QlibStateStore` timer reset race | 🔴 Critical | ✅ Resolved |
| CR-3 | `ModelPublisher` non-atomic publish | 🔴 Critical | ✅ Resolved |
| WR-1 | env var injection | 🟡 Warning | ✅ Resolved |
| WR-2 | missing index | 🟡 Warning | ✅ Resolved |
| WR-3 | prepare-per-call | 🟡 Warning | ✅ Resolved |
| WR-4 | barsPerYear silent fallback | 🟡 Warning | ✅ Resolved |
| WR-5 | batch busy-poll | 🟡 Warning | ✅ Resolved |
| IN-1 | Shadow vs Disabled fallback | ⚪ Info | ✅ Resolved |
| IN-2 | sha256File partial read | ⚪ Info | ✅ Resolved |
| IN-3 | missing `<algorithm>` | ⚪ Info | ✅ Resolved |
| IN-4 | error message wording | ⚪ Info | ✅ Improved |
| NW-1 | execvp 127 vs child 127 ambiguity (new) | — | ✅ Resolved |

**Verdict:** **Approve.** All critical, warning, info, and follow-up findings in the orchestration review scope are fixed or improved. The module is production-ready for the orchestration scope.
