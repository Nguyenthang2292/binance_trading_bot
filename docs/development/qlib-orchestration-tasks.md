# Qlib Orchestration - Implementation Task List

**Design reference:** `docs/design/2026-05-20-qlib-orchestration-v1.1.md`  
**Data + orchestration modules:** `src/orchestration/`

**Status legend:** `[ ]` todo · `[~]` in progress · `[x]` done

---

## Phase A - ProcessManager

- [x] `src/orchestration/process_manager.h`
- [x] `src/orchestration/process_manager.cpp`
- [x] `tests/test_process_manager.cpp`

Implemented:
- CreateProcess + stdout/stderr capture + timeout/terminate
- exponential backoff retry
- per-attempt log file output
- Windows-safe command quoting + POSIX fallback

---

## Phase B - BatchSchedulerThread (Phase 1 -> Phase 2)

- [x] `src/orchestration/batch_scheduler_thread.h`
- [x] `src/orchestration/batch_scheduler_thread.cpp`
- [x] `tests/test_batch_scheduler_thread.cpp`

Implemented:
- weekday/time gate
- once-per-day guard
- Phase 1 -> Phase 2 chain
- publish model after Phase 2 success
- log pruning
- `runScheduledCycleAt(...)` for deterministic schedule tests
- stop-aware minute waiting loop

---

## Phase C - CandleSchedulerThread (Phase 3)

- [x] `src/orchestration/candle_scheduler_thread.h`
- [x] `src/orchestration/candle_scheduler_thread.cpp`
- [x] `tests/test_candle_scheduler_thread.cpp`

Implemented:
- thread-safe candle-close queue
- latest-candle coalescing + duplicate suppression
- post-candle delay
- Phase 3 command build and process run
- Phase 4 trigger only after Phase 3 success

---

## Phase D - PromotionChecker (Phase 4)

- [x] `src/orchestration/promotion_checker.h`
- [x] `src/orchestration/promotion_checker.cpp`
- [x] `tests/test_promotion_checker.cpp`

Implemented:
- shadow outcome stats query (lookback window)
- Sharpe + hit-rate evaluation
- promotion state machine:
  - Shadow -> LiveCanary
  - LiveCanary -> Live
  - Live -> AlreadyLive

---

## Phase E - Wiring & Config

- [x] `src/orchestration/orchestrator_config.h`
- [x] `src/main.cpp` wiring (state store, shadow recorder, schedulers, callback hooks)
- [x] `CMakeLists.txt` source/header integration

Note:
- tests are included via `file(GLOB TEST_SOURCES tests/*.cpp)` and now include:
  - `test_process_manager.cpp`
  - `test_batch_scheduler_thread.cpp`
  - `test_candle_scheduler_thread.cpp`
  - `test_promotion_checker.cpp`

---

## Review Findings Fix Status

- [x] Silent runtime-state UPDATE failure check (`sqlite3_changes`)
- [x] Cross-filesystem staging move fallback (`rename` -> `copy + remove_all`)
- [x] Reload timer lifetime race (`weak_from_this`)
- [x] SQLite helper dedup (`sqlite_helpers.h`)
- [x] Added index `idx_shadow_signals_asof`
- [x] Prediction lookup filter by `asof_open_time_ms`
- [x] Replaced hardcoded unknown hashes with computed hash values
- [x] Removed unused selected column in shadow outcomes query
- [x] Runtime snapshot stored directly (not shared_ptr)
- [x] `busy_timeout=5000` on DB connections used in orchestration components

---

## Phase F - Integration Smoke Test

- [ ] Manual end-to-end run with `qlib_orchestration.enabled=true` in runtime config
- [ ] Verify Phase 1/2/3 scheduling in real bot loop
- [ ] Verify runtime state transition in live shadow outcomes

---

## Validation Run (current)

- [x] `cmake --build build --config Debug -j 4`
- [x] `ctest --test-dir build -C Debug -R "BatchSchedulerThreadTest|CandleSchedulerThreadTest|ProcessManagerTest|PromotionCheckerTest" --output-on-failure`
