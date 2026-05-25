# Backtest Range Proposer

This module is invoked by the `BacktestGateController` as a subprocess to propose optimized parameter search ranges for walk-forward optimization.

## Setup

Reuses the same Python environment as `tools/gemini_filter`. No additional venv is needed.

### Environment Variables

This tool uses the same Gemini key management as `tools/gemini_filter`.
Set one or more of the following in `.env` at the project root (or export in your shell):

| Variable | Description |
|----------|-------------|
| `GEMINI_API_KEY_1`, `GEMINI_API_KEY_2`, ... | Numbered API keys |
| `GEMINI_API_KEY` | Single API key |
| `GEMINI_API_KEYS` | Comma- or semicolon-separated list of keys |
| `GEMINI_TEXT_API_KEY` | Text-key alias |
| `GEMINI_KEY_MANAGER_STATE_DIR` | Optional shared round-robin state directory; defaults to `tmp/gemini_key_manager` |

At least one key variable above must be set. The key manager combines all configured sources,
deduplicates repeated key values, and uses a shared round-robin cursor for every Gemini call
made by `tools/gemini_filter` and `tools/backtest_range_proposer`. Retryable key/API errors
continue with the next key in the same round-robin order.

## Usage

```bash
python -m tools.backtest_range_proposer.main <input_json> [<output_json>]
```

- `<input_json>` — path to input JSON (schema below)
- `<output_json>` — optional output path; prints to **stdout** if omitted

## Input JSON Schema

```json
{
  "eval_id": "550e8400-...",
  "symbol": "BTCUSDT",
  "interval": "4h",
  "strategy_id": "golden_crossover",
  "tunable_params": ["ma_short", "ma_long", "atr_period", "sl_multiplier", "tp_multiplier"],
  "current_values": {
    "ma_short": 50, "ma_long": 200, "atr_period": 14,
    "sl_multiplier": 1.5, "tp_multiplier": 3.0
  },
  "prompt_context_aggregates": {
    "ret_30d_pct": 12.5,
    "atr_pct_current": 2.3,
    "avg_volume_usd": 850000000,
    "trend_direction": "up",
    "realized_vol_30d": 0.42,
    "num_candles": 500
  },
  "default_ranges": [
    {"name": "ma_short",      "min": 10,  "max": 80,  "step": 5,    "is_integer": true},
    {"name": "ma_long",       "min": 50,  "max": 250, "step": 10,   "is_integer": true},
    {"name": "atr_period",    "min": 7,   "max": 21,  "step": 1,    "is_integer": true},
    {"name": "sl_multiplier", "min": 1.0, "max": 3.0, "step": 0.25, "is_integer": false},
    {"name": "tp_multiplier", "min": 2.0, "max": 5.0, "step": 0.25, "is_integer": false}
  ],
  "model": "gemini-2.0-flash"
}
```

> **Important:** `prompt_context_aggregates` must contain stats derived ONLY from the `prompt_context` slice of historical data (the oldest 50% of the calibration window). The C++ caller enforces this — the Python module receives only pre-computed aggregates, so no calibration or OOS data leaks into the prompt.

## Output JSON Schema (success)

```json
{
  "eval_id": "550e8400-...",
  "ranges": [
    {"name": "ma_short",      "min": 20,  "max": 60,  "step": 5,    "is_integer": true},
    {"name": "ma_long",       "min": 100, "max": 220, "step": 10,   "is_integer": true},
    {"name": "atr_period",    "min": 10,  "max": 18,  "step": 1,    "is_integer": true},
    {"name": "sl_multiplier", "min": 1.25,"max": 2.5, "step": 0.25, "is_integer": false},
    {"name": "tp_multiplier", "min": 2.5, "max": 4.5, "step": 0.25, "is_integer": false}
  ],
  "notes": "Trending market with high ATR suggests wider MA ranges and tighter stops."
}
```

## Output JSON Schema (error)

```json
{
  "eval_id": "...",
  "error": true,
  "error_code": "gemini_error | invalid_input | invalid_response | usage_error",
  "reason": "human-readable error message",
  "details": "exception message"
}
```

Exit code is **0** on success, **1** on error.

## Offline Testing

```bash
python -m pytest tests/python/test_backtest_range_proposer.py -q
```
