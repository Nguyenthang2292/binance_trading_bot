#pragma once

#include "strategy/strategy_registry.h"

#include <string>
#include <vector>

namespace engine {

struct WorkItem {
    std::string symbol;
    std::string interval;
    const strategy::IStrategy* strategy{nullptr};
};

class WorkQueue {
public:
    static std::vector<WorkItem> build(
        const std::vector<std::string>& symbols,
        const strategy::StrategyRegistry& registry);
};

} // namespace engine

