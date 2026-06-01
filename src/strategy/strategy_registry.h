#pragma once

#include "strategy/istrategy.h"

#include <memory>
#include <shared_mutex>
#include <string_view>
#include <vector>

namespace strategy {

class StrategyRegistry {
public:
    void add(std::unique_ptr<IStrategy> strategy);
    void addShared(std::shared_ptr<IStrategy> strategy);
    void clear();

    std::vector<const IStrategy*> all() const;
    std::vector<const IStrategy*> forInterval(std::string_view interval) const;
    std::vector<std::shared_ptr<const IStrategy>> allShared() const;
    std::vector<std::shared_ptr<const IStrategy>> forIntervalShared(std::string_view interval) const;

private:
    std::vector<std::shared_ptr<IStrategy>> m_strategies;
    mutable std::shared_mutex m_mutex;
};

} // namespace strategy
