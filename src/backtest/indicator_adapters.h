#pragma once

#include "backtest/optimizable_strategy.h"

#include <string_view>
#include <vector>

namespace backtest {

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
