# Qlib Runtime State Runbook

## Purpose

This runbook describes how Qlib live/shadow state is keyed and initialized.

## Runtime State Sources

| Strategy type | Runtime table | Key | Owner |
|---|---|---|---|
| `qlib_model_signal` | `qlib_runtime_state` | `(model_id, interval)` | legacy model signal path |
| `qlib_strategy_signal` | `qlib_adapter_runtime_state` | `(adapter_id, interval)` | adapter-aware strategy path |

`qlib_model_signal` keeps the legacy model-level state path. `qlib_strategy_signal` must use an adapter-level state row so multiple adapters can run on the same model and interval with independent modes.

## Adapter Id Contract

For `qlib_strategy_signal`, the canonical adapter id is:

```json
{
  "type": "qlib_strategy_signal",
  "name": "Human readable display name",
  "params": {
    "strategy_id": "topk_dropout_30m_v1"
  }
}
```

The bot uses `params.strategy_id` as the SQLite `adapter_id`. The display `name` is only for logs and catalog output. If `params.strategy_id` changes, the adapter uses a different runtime-state row.

## Automatic Seeding

At startup, when `qlib_orchestration.enabled=true`, the bot creates missing runtime rows:

- `qlib_runtime_state` for the configured model id and interval.
- `qlib_adapter_runtime_state` for every configured `qlib_strategy_signal`.
- `qlib_adapter_runtime_state` for every `qlib_orchestration.adapters[]` entry.

Existing rows are preserved. Startup seeding uses `ON CONFLICT DO NOTHING`, so operator changes in SQLite are not overwritten.

## Default Mode

The startup default mode comes from config and is applied only when the row does not already exist.

For `qlib_model_signal`:

```json
"execution": {
  "mode_source": "sqlite",
  "default_mode": "live"
}
```

For `qlib_strategy_signal`:

```json
"execution": {
  "default_mode": "live"
}
```

Supported modes:

- `disabled`
- `shadow`
- `shadow_only`
- `live_canary`
- `live`

## Promotion Profiles

Adapter rows also carry `promotion_profile`. Startup seeding reads it from:

1. `params.promotion_profile`
2. top-level `promotion_profile`
3. `execution.promotion_profile`
4. fallback `default`

Promotion reads thresholds from `qlib_promotion_profiles.profile_json`. Missing or invalid named profiles block promotion and write a `profile_error` evaluation row.

## Manual Checks

Inspect model-level state:

```sql
SELECT model_id, interval, execution_mode, active_run_id, state_version
FROM qlib_runtime_state;
```

Inspect adapter-level state:

```sql
SELECT adapter_id, interval, execution_mode, promotion_profile, active_run_id, state_version
FROM qlib_adapter_runtime_state;
```

Inspect promotion decisions:

```sql
SELECT profile_name, interval, execution_mode, mature_signals, hit_rate, sharpe,
       mean_net_return_bps, decision, reason
FROM qlib_promotion_evaluations
ORDER BY evaluated_at_ms DESC
LIMIT 20;
```

## Smoke Verification

Use non-trading verification first:

```powershell
rtk .\build\bin\Debug\binance_trading_bot.exe --list-strategies
rtk .\build\bin\Debug\binance_trading_bot_tests.exe --gtest_filter=SignalEngineTest.Qlib*:QlibStateStoreTest.*:PromotionCheckerTest.*
```

Do not run a live bot loop for smoke unless the selected config is intentionally live.
