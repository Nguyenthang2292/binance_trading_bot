#pragma once

#include "strategy/istrategy.h"
#include "types/account.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine {

struct TrackedPosition {
    std::string symbol;
    strategy::Signal::Direction direction{strategy::Signal::Direction::None};
    std::chrono::system_clock::time_point openedAt{};
    std::chrono::seconds maxHoldDuration{0};
    double entryPrice{0.0};
    double quantity{0.0};
    int64_t tpOrderId{0};
    int64_t slOrderId{0};
    std::string tpClientOrderId;
    std::string slClientOrderId;
    bool trailingEnabled{false};
    std::string trailingInterval;
    int trailingCandles{0};
    std::chrono::seconds trailingCheckInterval{0};
    double currentTrailLevel{0.0};
};

class PositionTracker {
public:
    void loadFromSnapshot(const std::vector<Position>& positions);

    void add(TrackedPosition pos);
    void remove(std::string_view symbol);
    bool has(std::string_view symbol) const;

    std::vector<TrackedPosition> expired(std::chrono::system_clock::time_point now) const;
    std::vector<TrackedPosition> all() const;
    bool removeByExitOrderClientId(std::string_view clientOrderId);
    std::optional<TrackedPosition> bySymbol(std::string_view symbol) const;
    bool updateStopLoss(
        std::string_view symbol,
        int64_t slOrderId,
        std::string slClientOrderId,
        double currentTrailLevel);

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, TrackedPosition> m_positions;
};

} // namespace engine
