#pragma once

#include "strategy/strategy_config.h"
#include "types/market.h"

#include <string>
#include <string_view>
#include <vector>

namespace strategy {

struct Signal {
    enum class Direction { Long, Short, None };

    Direction direction{Direction::None};
    double confidence{0.0};
    double atr{0.0};
    std::string reason;
};

class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual const StrategyConfig& config() const = 0;
    virtual Signal evaluate(
        std::string_view symbol,
        std::string_view interval,
        const std::vector<Kline>& klines) const = 0;
};

} // namespace strategy

