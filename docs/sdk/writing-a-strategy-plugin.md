# Writing a Strategy Plugin DLL

This SDK guide explains how to build a strategy plugin DLL for the bot catalog.

## 1. Required exports

Your DLL must export these 4 C ABI functions:

```cpp
extern "C" {
__declspec(dllexport) strategy::IStrategy* createStrategy(const char* config_json);
__declspec(dllexport) void destroyStrategy(strategy::IStrategy* strategy);
__declspec(dllexport) const char* strategyType();
__declspec(dllexport) const char* pluginVersion();
}
```

Rules:
- Do not throw exceptions across the ABI boundary.
- Return `nullptr` from `createStrategy` when config is invalid.
- Always release strategy instances using `destroyStrategy`.
- Keep `strategyType()` stable, because host config matches by this string.

## 2. Config contract

Host forwards each `strategies[]` config entry as raw JSON string to `createStrategy`.

Typical config:

```json
{
  "name": "rsi_reversal_15m",
  "type": "rsi_reversal",
  "intervals": ["15m"],
  "scan_interval_seconds": 3600,
  "max_hold_duration_seconds": 86400,
  "risk_pct": 0.01,
  "sl_multiplier": 1.5,
  "tp_multiplier": 3.0,
  "min_notional": 1.0,
  "atr_period": 14,
  "min_confidence": 0.5,
  "trailing_enabled": false,
  "trailing_interval": "15m",
  "trailing_candles": 0,
  "trailing_check_interval_seconds": 300,
  "params": {
    "rsi_period": 14
  }
}
```

Generic trailing stop support is exposed through `StrategyConfig::trailingStop`.
If your strategy wants engine-managed trailing stops, parse the JSON fields above
and copy them into `cfg.trailingStop`. The engine reads `StrategyConfig`; it does
not inspect raw strategy params.

## 3. Build template

Use the template at `docs/sdk/template_plugin/`.

Build steps (Windows, MSVC):
1. Configure: `cmake -S docs/sdk/template_plugin -B build/template_plugin -G "Visual Studio 17 2022" -A x64`
2. Build: `cmake --build build/template_plugin --config Release`
3. Copy generated DLL to `plugins/`
4. Run host with `--list-strategies` to verify load

## 4. Runtime checklist

- `strategyType()` equals `strategies[].type`
- `config().intervals` is not empty
- `evaluate()` returns `Signal::Direction::None` when no signal
- `Signal.atr > 0` when strategy expects engine TP/SL sizing from strategy ATR
- `config().trailingStop.enabled` is set only when the strategy expects the generic engine trailing stop monitor to manage SL movement

