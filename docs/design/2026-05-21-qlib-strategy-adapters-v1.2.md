# Qlib Strategy Adapter Integration Design - v1.2

**Date:** 2026-05-21  
**Status:** IMPLEMENTED  
**Audience:** AI agents, human developers

This document supersedes `docs/design/2026-05-21-qlib-strategy-adapters-v1.1.md` for the implemented state of the Qlib strategy adapter work.

## 1. What Changed From v1.1

v1.1 intentionally deferred a few items after the first shadow-only implementation. v1.2 closes the remaining implementation/design gaps:

| Area | v1.1 status | v1.2 status |
|---|---|---|
| TWAP per-slice execution | Deferred to Phase 8 | Implemented in `QlibExecutionPlanner` |
| Per-slice revocation | Deferred to Phase 8 | Implemented by re-querying latest `qlib_strategy_decisions` before each slice |
| Per-request TWAP plan shape | Deferred | Implemented in `run_execution_plan_watcher.py` via `metadata_json` |
| Promotion profiles | Deferred | Schema support added via `qlib_promotion_profiles` and runtime-state `promotion_profile` column |
| Runtime-state schema mismatch | Design/doc gap | Reconciled: implementation keeps `active_run_id` + `state_version` and adds promotion/audit columns |
| Execution slice PK mismatch | Design/doc gap | Reconciled: `slice_id TEXT PRIMARY KEY` is the implemented contract |
| Qlib class invocation | Open concern | Documented as stateless algorithm port with allowlisted import verification |

## 2. Implemented Execution Contract

`QlibExecutionPlanner` now follows this flow:

1. C++ writes a `qlib_execution_requests` row with `status='pending'`.
2. The Python watcher writes one `qlib_execution_plans` row and one or more `qlib_execution_slices` rows in a single transaction.
3. C++ polls for the latest `qlib_execution_plans.status='succeeded'` plan before the request deadline.
4. If no plan is ready before the short C++ deadline, the planner uses the injected native fallback when configured.
5. If a plan is ready, C++ executes each pending slice sequentially:
   - waits until `due_at_ms`
   - validates side and cumulative quantity against the approved parent order
   - parses slice quantity through `DecimalString`
   - re-queries latest `qlib_strategy_decisions` when metadata contains `strategy_tag` and `timeframe`
   - revokes remaining pending slices if the current decision no longer supports `buy/long`
   - submits the slice via the injected `IExecutionPlanner`
   - marks accepted slices as `filled`; failed or rejected slices as `failed`

The injected fallback is now both:

- the native fallback for plan timeout in shadow-only rollout, and
- the single-slice submission mechanism used by the TWAP slice executor.

## 3. Revocation Contract

Revocation is evaluated immediately before every slice submission.

For v1 long-only alpha adapters, a slice is valid only if the latest decision for `(strategy_id, symbol, interval)` is still:

```text
action='buy' AND direction='long'
```

If the latest succeeded decision is `none/none`, `hold/none`, or any non-buy/non-long value, the planner:

1. marks all remaining pending slices for the plan as `status='revoked'`
2. sets `revoked_at_ms`
3. writes `revoke_reason`, for example `direction_reversed action=none direction=none`
4. returns a failed planner result instead of submitting a new slice

If order metadata lacks `strategy_tag` or `timeframe`, revocation re-query is skipped because there is no safe adapter key. This preserves compatibility for non-Qlib strategies that accidentally flow through the planner.

## 4. SQLite Schema Reconciliation

The implemented schema version remains `PRAGMA user_version = 7`.

### `qlib_execution_slices`

Implemented primary key:

```sql
slice_id TEXT PRIMARY KEY
```

This replaces the older design idea of a composite `(plan_id, slice_index)` key. `slice_index` remains present and ordered, but `slice_id` is the stable mutation target for status updates.

### `qlib_adapter_runtime_state`

The implemented schema combines optimistic concurrency fields with promotion/audit fields:

```sql
CREATE TABLE IF NOT EXISTS qlib_adapter_runtime_state (
    adapter_id          TEXT NOT NULL,
    interval            TEXT NOT NULL,
    execution_mode      TEXT NOT NULL CHECK (
        execution_mode IN ('disabled','shadow','shadow_only','live_canary','live')
    ),
    promotion_profile   TEXT NOT NULL DEFAULT 'default',
    active_run_id       TEXT,
    state_version       INTEGER NOT NULL DEFAULT 0,
    promoted_at_ms      INTEGER,
    promoted_by         TEXT,
    last_decision_at_ms INTEGER,
    last_failure_at_ms  INTEGER,
    last_failure_reason TEXT,
    updated_at_ms       INTEGER NOT NULL,
    rollback_reason     TEXT,
    PRIMARY KEY (adapter_id, interval)
);
```

`active_run_id` and `state_version` are kept because they are useful for runtime safety and optimistic state updates. Promotion fields are added so adapter promotion decisions can be audited without changing the legacy `qlib_runtime_state` table.

### `qlib_promotion_profiles`

Promotion profile schema support:

```sql
CREATE TABLE IF NOT EXISTS qlib_promotion_profiles (
    profile_name        TEXT PRIMARY KEY,
    qlib_class          TEXT NOT NULL,
    profile_json        TEXT NOT NULL,
    updated_at_ms       INTEGER NOT NULL
);
```

`run_strategy.py` upserts a profile when `config_json` contains:

```json
{
  "promotion_profile": "alpha_topk_dropout_default",
  "promotion_profile_config": {
    "min_shadow_signals": 200
  }
}
```

## 5. Qlib Strategy Algorithm Integration

`run_strategy.py` intentionally does not call `generate_trade_decision()` directly.

Reason: Qlib's concrete strategy classes require the full Qlib trade runtime: signal wrapper, trade calendar, exchange, executor, account/position objects, and level/common infra. That harness does not map cleanly to this bot's per-run SQLite sidecar contract.

The implemented contract is:

- class path must pass the hardcoded allowlist
- class import is attempted and recorded for deployment-time verification
- TopkDropout and SoftTopk algorithms are ported statelessly against `qlib_predictions` and previous `qlib_strategy_targets`

Implemented behavior:

- `TopkDropoutStrategy`: honors `topk` / `k` and `n_drop`
- `SoftTopkStrategy`: honors `topk`, `max_sold_weight`, `trade_impact_limit`, and `buy_method`
- `FileOrderStrategy`: remains replay/test only

## 6. Acceptance Criteria Status

| # | Criterion | Status |
|---|---|---|
| 1 | TopkDropout decisions generated through hardcoded allowlist | Done |
| 2 | `qlib_strategy_signal` emits valid `strategy::Signal` | Done |
| 3 | SignalEngine handles `qlib_strategy_signal` as Qlib-managed | Done |
| 4 | Shadow metrics include blocked stage + adapter id | Done |
| 5 | Legacy `qlib_model_signal` remains compatible | Done |
| 6 | Existing non-Qlib strategies retain native behavior | Done |
| 7 | TWAP planner can operate without synchronous Python | Done |
| 8 | Invalid/stale artifacts cannot place live orders | Done |
| 9 | Decision Arbiter resolves conflicts deterministically | Done |
| 10 | SQLite contract verified through `user_version`, WAL, busy timeout | Done |
| 11 | TWAP per-slice revocation marks pending slices `revoked` | Done |

## 7. Verification Added

New tests cover:

- `QlibExecutionPlannerTest.ExecutesReadyPlanSlices`
- `QlibExecutionPlannerTest.RevokesPendingSlicesWhenLatestDecisionContradicts`
- `test_topk_dropout_respects_n_drop`
- `test_soft_topk_limits_turnover`
- `test_promotion_profile_is_recorded`

The expected verification commands are:

```powershell
rtk .\.venv\Scripts\python.exe -m pytest -q tests/python/test_run_strategy.py
rtk cmake --build build --config Debug --target binance_trading_bot_tests -j 4
rtk .\build\bin\Debug\binance_trading_bot_tests.exe --gtest_filter=QlibExecutionPlannerTest.*
rtk ctest --test-dir build -C Debug --output-on-failure
```
