#include "strategy/strategy_registry.h"

#include <algorithm>
#include <mutex>

namespace strategy {

void StrategyRegistry::add(std::unique_ptr<IStrategy> strategy) {
    if (!strategy) {
        return;
    }
    std::unique_lock lock(m_mutex);
    m_strategies.emplace_back(std::move(strategy));
}

void StrategyRegistry::addShared(std::shared_ptr<IStrategy> strategy) {
    if (!strategy) {
        return;
    }
    std::unique_lock lock(m_mutex);
    m_strategies.push_back(std::move(strategy));
}

void StrategyRegistry::clear() {
    std::unique_lock lock(m_mutex);
    m_strategies.clear();
}

std::vector<const IStrategy*> StrategyRegistry::all() const {
    const auto snapshot = allShared();
    std::vector<const IStrategy*> out;
    out.reserve(snapshot.size());
    for (const auto& strategy : snapshot) {
        out.push_back(strategy.get());
    }
    return out;
}

std::vector<const IStrategy*> StrategyRegistry::forInterval(std::string_view interval) const {
    const auto snapshot = forIntervalShared(interval);
    std::vector<const IStrategy*> out;
    out.reserve(snapshot.size());
    for (const auto& strategy : snapshot) {
        out.push_back(strategy.get());
    }
    return out;
}

std::vector<std::shared_ptr<const IStrategy>> StrategyRegistry::allShared() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::shared_ptr<const IStrategy>> out;
    out.reserve(m_strategies.size());
    for (const auto& strategy : m_strategies) {
        out.push_back(strategy);
    }
    return out;
}

std::vector<std::shared_ptr<const IStrategy>> StrategyRegistry::forIntervalShared(std::string_view interval) const {
    std::shared_lock lock(m_mutex);
    std::vector<std::shared_ptr<const IStrategy>> out;
    for (const auto& strategy : m_strategies) {
        const auto& intervals = strategy->config().intervals;
        if (std::find(intervals.begin(), intervals.end(), interval) != intervals.end()) {
            out.push_back(strategy);
        }
    }
    return out;
}

} // namespace strategy
