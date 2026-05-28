/**
 * @file optimizable_strategy.h
 * @brief Interface for strategy adapters that expose tunable parameter spaces.
 */

#pragma once

#include "backtest/parameter_space.h"
#include "strategy/istrategy.h"
#include "strategy/strategy_config.h"
#include "types/market.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace backtest {

// Note v1.1 limitation: `currentValues` is populated from `StrategyConfig` for
// fields that exist on it (atrPeriod, slMultiplier, tpMultiplier, minConfidence).
// Plugin-specific tunables (ma_short, ma_long, short_period, ...) are NOT on
// StrategyConfig, so the adapter populates them with the plugin's BUILT-IN
// defaults — not the runtime overrides from `params:` in config.json. This is
// acceptable because Gemini uses currentValues only as a contextual hint, and
// the actual sweep uses the proposed ranges (which span well beyond defaults).
// A v1.2 ABI change is required to thread overridden params through here.
/**
 * @brief Parameter-space contract provided by an optimizable strategy adapter.
 */
struct StrategyParamSpec {
    std::vector<std::string> tunableParams;
    std::vector<ParamRange> defaults;
    std::vector<ParamConstraint> constraints;
    std::unordered_map<std::string, double> currentValues;
};

/**
 * @brief Adapter interface used by the backtest gate for parameterized signals.
 */
class IOptimizableStrategy {
public:
    virtual ~IOptimizableStrategy() = default;

    /**
     * @brief Returns tunable ranges, constraints, and contextual current values.
     */
    virtual StrategyParamSpec spec(const strategy::StrategyConfig& base) const = 0;

    // `klines` are closed bars only, ending at the evaluation bar.
    // `baseConfig` provides cfg.atrPeriod, cfg.minConfidence etc. that are
    // not part of `params` but are needed to faithfully reproduce live behavior.
    /**
     * @brief Evaluates strategy signal with a specific parameter point.
     */
    virtual strategy::Signal evaluateWith(
        std::string_view symbol,
        std::string_view interval,
        const std::vector<Kline>& klines,
        const ParamPoint& params,
        const strategy::StrategyConfig& baseConfig) const = 0;
};

}  // namespace backtest
