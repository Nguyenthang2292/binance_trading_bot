# Plugin Review Checklist

**Scope:** This checklist MUST be completed for every PR that modifies a strategy plugin under `plugins/src/<strategy_name>/` whose type is whitelisted by the BacktestGate.

**Whitelisted strategies (v1.1):**
- `golden_crossover`
- `donchian_5_20_crossover`
- `gartley_day_crossover`

**Why this matters:** The BacktestGate uses lightweight C++ adapters in `src/backtest/indicator_adapters.cpp` that re-implement plugin formulas WITHOUT going through the plugin ABI. If a plugin's formula or tunable shape changes and the corresponding adapter is not updated, the gate will silently optimize over the wrong signal surface — every gate evaluation becomes misleading.

The parity tests in `tests/test_indicator_adapters.cpp` catch divergence at build time, but they can only catch what they exercise. The checklist below is the human cross-check that complements them.

---

## PR Checklist

Tick every box before requesting review.

### 1. Formula sync

- [ ] If the plugin's `evaluate()` signal formula changed (indicator math, confidence calc, direction logic, ATR usage, minimum-candles guard), update the matching adapter's `evaluateWith()` in `src/backtest/indicator_adapters.cpp`.
- [ ] If the plugin added or removed any parameter handled in `parseParams()`, update the adapter's `StrategyParamSpec`:
  - Add/remove the entry from `tunableParams`.
  - Add/remove the entry from `defaults` with a sensible search range.
  - Add/remove the entry from `currentValues` matching the plugin's built-in default.
  - Add/remove any `constraints` (e.g., `fast < slow`).
- [ ] If the plugin changed param defaults (e.g., `ma_short` 50 → 30), update the adapter's `currentValues` to match.
- [ ] If the plugin changed param VALIDATION rules (e.g., now requires `ma_short >= 5`), reflect them in the adapter's parameter range bounds.

### 2. Tests

- [ ] Plugin tests in `tests/test_<strategy>_plugin.cpp` still pass.
- [ ] `tests/test_indicator_adapters.cpp` parity tests still pass. If they fail because the new behavior is intentional, update the parity assertion AND document the formula divergence under "Known divergences" below.
- [ ] If the change affects signal shape (e.g., new confidence formula), add or refresh a dataset in the parity test that covers the changed regime.

### 3. Range sanity check

When adding or modifying a tunable's range in the adapter's `defaults`:

- [ ] The range covers the plugin's documented default (currentValues falls inside [min, max]).
- [ ] The range is plausible for live trading: not so wide that 90% of combos will never produce a trade; not so narrow that Gemini cannot widen it usefully.
- [ ] Integer/non-integer flag matches the plugin's interpretation.
- [ ] Constraints between params (e.g., `ma_short < ma_long`) are declared.

### 4. Backward compatibility

- [ ] The plugin's `pluginVersion()` string is bumped if the change is observable (formula or default change).
- [ ] If the formula change is breaking, mention it in the PR description so the BacktestGate shadow rollout window can be re-run before live enforcement.

### 5. Documentation

- [ ] If the strategy doc under `docs/strategies/<strategy>.md` describes specific defaults or formulas, update it.
- [ ] If you added a tunable, note it in the strategy doc's "Parameters" section.

---

## Quick diff guide

When you touch a plugin file, also touch the adapter — even if just to add a comment confirming review:

| Plugin file | Adapter to review |
|---|---|
| `plugins/src/golden_crossover/strategy_golden_crossover.cpp` | `GoldenCrossoverAdapter` in `src/backtest/indicator_adapters.cpp` |
| `plugins/src/donchian_5_20_crossover/strategy_donchian_5_20_crossover.cpp` | `Donchian520CrossoverAdapter` |
| `plugins/src/gartley_day_crossover/strategy_gartley_day_crossover.cpp` | `GartleyDayCrossoverAdapter` |

If a NEW plugin is added to the BacktestGate whitelist, add:

- a new adapter class implementing `IOptimizableStrategy` in `src/backtest/indicator_adapters.{h,cpp}`,
- a parity test block in `tests/test_indicator_adapters.cpp`,
- a row in the table above, AND
- the strategy type to `isBacktestEligible()` in `src/engine/signal_engine.cpp` (Phase 4 wiring).

---

## Known divergences (intentional)

Track any deliberate plugin/adapter mismatches here so reviewers don't try to "fix" them.

_(None at v1.1.)_

---

**See also:** [docs/design/2026-05-23-backtest-parameter-optimizer-v1.1.md](../design/2026-05-23-backtest-parameter-optimizer-v1.1.md), section 13 (Risks & Mitigations — "Adapter drift from plugin logic").
