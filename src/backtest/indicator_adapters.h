/**
 * @file indicator_adapters.h
 * @brief Strategy adapters that mirror plugin formulas for parameter sweeps.
 */

#pragma once

#include "backtest/optimizable_strategy.h"

#include <string_view>
#include <vector>

namespace backtest {

/**
 * @brief Adapter for golden-crossover plugin signal logic.
 */
class GoldenCrossoverAdapter : public IOptimizableStrategy {
public:
    StrategyParamSpec spec(const strategy::StrategyConfig& base) const override;
    strategy::Signal evaluateWith(
        std::string_view symbol,
        std::string_view interval,
        const std::vector<Kline>& klines,
        const ParamPoint& params,
        const strategy::StrategyConfig& baseConfig) const override;
};

/**
 * @brief Adapter for donchian_5_20_crossover plugin signal logic.
 */
class Donchian520CrossoverAdapter : public IOptimizableStrategy {
public:
    StrategyParamSpec spec(const strategy::StrategyConfig& base) const override;
    strategy::Signal evaluateWith(
        std::string_view symbol,
        std::string_view interval,
        const std::vector<Kline>& klines,
        const ParamPoint& params,
        const strategy::StrategyConfig& baseConfig) const override;
};

/**
 * @brief Adapter for gartley_day_crossover plugin signal logic.
 */
class GartleyDayCrossoverAdapter : public IOptimizableStrategy {
public:
    StrategyParamSpec spec(const strategy::StrategyConfig& base) const override;
    strategy::Signal evaluateWith(
        std::string_view symbol,
        std::string_view interval,
        const std::vector<Kline>& klines,
        const ParamPoint& params,
        const strategy::StrategyConfig& baseConfig) const override;
};

}  // namespace backtest
