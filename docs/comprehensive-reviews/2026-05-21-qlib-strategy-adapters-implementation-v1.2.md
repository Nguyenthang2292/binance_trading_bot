# Qlib Strategy Adapter Integration - Implementation Review v1.2

**Date:** 2026-05-21  
**Status:** COMPLETE - Phase 8 slice execution and documentation reconciliation landed  
**Design:** `docs/design/2026-05-21-qlib-strategy-adapters-v1.2.md`

## 1. Summary

This update completes the remaining items left in review v1.1:

- TWAP slice execution is no longer deferred.
- Per-slice revocation is implemented and tested.
- Per-request watcher plan shape from `metadata_json` is implemented.
- Promotion profile schema support is implemented.
- Runtime-state and execution-slice schema deviations are documented in design v1.2.

## 2. Implementation Changes

### `QlibExecutionPlanner`

`src/engine/qlib_execution_planner.cpp` now:

- fetches the ready plan and all pending slices
- waits until each `due_at_ms`
- validates slice side and cumulative quantity
- parses slice quantity with `DecimalString`
- re-queries latest `qlib_strategy_decisions` using order metadata `strategy_tag` and `timeframe`
- revokes pending slices when the latest decision contradicts the parent buy/long order
- submits each slice through the injected `IExecutionPlanner`
- marks slices `submitted`, `filled`, `failed`, or `revoked`

### Python Schema And Promotion Profiles

`tools/qlib_bridge/run_strategy.py` now:

- adds promotion/audit columns to `qlib_adapter_runtime_state`
- creates `qlib_promotion_profiles`
- upserts a promotion profile when `config_json` contains `promotion_profile`
- preserves existing `PRAGMA user_version = 7`

### Watcher Plan Shape

`tools/qlib_bridge/run_execution_plan_watcher.py` supports per-request overrides from `metadata_json`:

- `twap.slice_count`
- `twap.duration_ms`
- `twap_slice_count`
- `twap_duration_ms`
- fallback aliases `slice_count` and `duration_ms`

## 3. Remaining Non-Blockers

No acceptance blocker remains from design v1.1.

Known future work:

- promotion decision engine that consumes `qlib_promotion_profiles`
- richer slice revalidation against exchange min quantity/min notional and live order-cap/exposure ports at each slice
- live canary rollout controls for execution adapters
- v2 alpha semantics for `close` / `short`

These are future product phases, not incomplete v1.2 requirements.

## 4. Verification

Added tests:

- `QlibExecutionPlannerTest.ExecutesReadyPlanSlices`
- `QlibExecutionPlannerTest.RevokesPendingSlicesWhenLatestDecisionContradicts`
- `test_promotion_profile_is_recorded`

Previously added and retained:

- `test_topk_dropout_respects_n_drop`
- `test_soft_topk_limits_turnover`

Expected verification:

```powershell
rtk .\.venv\Scripts\python.exe -m pytest -q tests/python/test_run_strategy.py
rtk cmake --build build --config Debug --target binance_trading_bot_tests -j 4
rtk .\build\bin\Debug\binance_trading_bot_tests.exe --gtest_filter=QlibExecutionPlannerTest.*
rtk ctest --test-dir build -C Debug --output-on-failure
```

## 5. Decision

Approved for merge as the implemented v1.2 Qlib strategy adapter baseline.
