#include "engine/position_tracker.h"

#include <cmath>

namespace engine {

void PositionTracker::loadFromSnapshot(const std::vector<Position>& positions) {
    std::lock_guard lock(m_mutex);
    m_positions.clear();
    for (const auto& position : positions) {
        if (std::abs(position.positionAmt) <= 0.0) {
            continue;
        }
        TrackedPosition tracked;
        tracked.symbol = position.symbol;
        tracked.direction = position.positionAmt >= 0.0 ? strategy::Signal::Direction::Long
                                                        : strategy::Signal::Direction::Short;
        tracked.entryPrice = position.entryPrice;
        tracked.openedAt = std::chrono::system_clock::now();
        tracked.maxHoldDuration = std::chrono::seconds{0};
        m_positions[tracked.symbol] = tracked;
    }
}

void PositionTracker::add(TrackedPosition pos) {
    std::lock_guard lock(m_mutex);
    m_positions[pos.symbol] = std::move(pos);
}

void PositionTracker::remove(std::string_view symbol) {
    std::lock_guard lock(m_mutex);
    m_positions.erase(std::string(symbol));
}

bool PositionTracker::has(std::string_view symbol) const {
    std::lock_guard lock(m_mutex);
    return m_positions.find(std::string(symbol)) != m_positions.end();
}

std::vector<TrackedPosition> PositionTracker::expired(std::chrono::system_clock::time_point now) const {
    std::lock_guard lock(m_mutex);
    std::vector<TrackedPosition> out;
    for (const auto& [_, pos] : m_positions) {
        if (pos.maxHoldDuration.count() <= 0) {
            continue;
        }
        if (now - pos.openedAt >= pos.maxHoldDuration) {
            out.push_back(pos);
        }
    }
    return out;
}

std::vector<TrackedPosition> PositionTracker::all() const {
    std::lock_guard lock(m_mutex);
    std::vector<TrackedPosition> out;
    out.reserve(m_positions.size());
    for (const auto& [_, pos] : m_positions) {
        out.push_back(pos);
    }
    return out;
}

bool PositionTracker::removeByExitOrderClientId(std::string_view clientOrderId) {
    std::lock_guard lock(m_mutex);
    for (auto it = m_positions.begin(); it != m_positions.end(); ++it) {
        if (it->second.tpClientOrderId == clientOrderId || it->second.slClientOrderId == clientOrderId) {
            m_positions.erase(it);
            return true;
        }
    }
    return false;
}

std::optional<TrackedPosition> PositionTracker::bySymbol(std::string_view symbol) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_positions.find(std::string(symbol));
    if (it == m_positions.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace engine

