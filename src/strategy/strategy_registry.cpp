#include "strategy/strategy_registry.h"

#include <algorithm>

namespace strategy {

void StrategyRegistry::add(std::unique_ptr<IStrategy> strategy) {
    if (!strategy) {
        return;
    }
    m_strategies.emplace_back(std::move(strategy));
}

void StrategyRegistry::addShared(std::shared_ptr<IStrategy> strategy) {
    if (!strategy) {
        return;
    }
    m_strategies.push_back(std::move(strategy));
}

void StrategyRegistry::clear() {
    m_strategies.clear();
}

std::vector<const IStrategy*> StrategyRegistry::all() const {
    std::vector<const IStrategy*> out;
    out.reserve(m_strategies.size());
    for (const auto& strategy : m_strategies) {
        out.push_back(strategy.get());
    }
    return out;
}

std::vector<const IStrategy*> StrategyRegistry::forInterval(std::string_view interval) const {
    std::vector<const IStrategy*> out;
    for (const auto& strategy : m_strategies) {
        const auto& intervals = strategy->config().intervals;
        if (std::find(intervals.begin(), intervals.end(), interval) != intervals.end()) {
            out.push_back(strategy.get());
        }
    }
    return out;
}

} // namespace strategy
