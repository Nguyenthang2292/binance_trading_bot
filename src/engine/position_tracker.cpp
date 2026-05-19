#include "engine/position_tracker.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine {

void PositionTracker::loadFromSnapshot(
    const std::vector<Position>& positions,
    std::chrono::seconds defaultMaxHoldDuration) {
    std::lock_guard lock(m_mutex);
    m_positions.clear();
    for (const auto& position : positions) {
        const double absQty = std::abs(position.positionAmt);
        if (absQty <= 0.0) {
            continue;
        }
        TrackedPosition tracked;
        tracked.symbol = position.symbol;
        tracked.direction = position.positionAmt >= 0.0 ? strategy::Signal::Direction::Long
                                                        : strategy::Signal::Direction::Short;
        tracked.entryPrice = position.entryPrice;
        tracked.quantity = absQty;
        tracked.openedAt = std::chrono::system_clock::now();
        tracked.maxHoldDuration = defaultMaxHoldDuration;
        tracked.openingInFlight = false;
        tracked.recoveredFromSnapshot = true;
        m_positions[tracked.symbol] = tracked;
    }
}

bool PositionTracker::reserve(std::string symbol) {
    std::lock_guard lock(m_mutex);
    if (symbol.empty()) {
        return false;
    }
    if (m_positions.find(symbol) != m_positions.end()) {
        return false;
    }

    TrackedPosition pending;
    pending.symbol = std::move(symbol);
    pending.openingInFlight = true;
    pending.openedAt = std::chrono::system_clock::now();
    m_positions[pending.symbol] = std::move(pending);
    return true;
}

bool PositionTracker::commitReserved(std::string_view symbol, TrackedPosition pos) {
    std::lock_guard lock(m_mutex);
    const auto it = m_positions.find(std::string(symbol));
    if (it == m_positions.end() || !it->second.openingInFlight) {
        return false;
    }
    pos.openingInFlight = false;
    m_positions[pos.symbol] = std::move(pos);
    return true;
}

bool PositionTracker::add(TrackedPosition pos) {
    std::lock_guard lock(m_mutex);
    if (m_positions.find(pos.symbol) != m_positions.end()) {
        return false;
    }
    pos.openingInFlight = false;
    m_positions[pos.symbol] = std::move(pos);
    return true;
}

void PositionTracker::remove(std::string_view symbol) {
    std::lock_guard lock(m_mutex);
    m_positions.erase(std::string(symbol));
}

bool PositionTracker::removeIfOpenedAt(std::string_view symbol, std::chrono::system_clock::time_point openedAt) {
    std::lock_guard lock(m_mutex);
    const auto it = m_positions.find(std::string(symbol));
    if (it == m_positions.end()) {
        return false;
    }
    if (it->second.openedAt != openedAt) {
        return false;
    }
    m_positions.erase(it);
    return true;
}

bool PositionTracker::has(std::string_view symbol) const {
    std::lock_guard lock(m_mutex);
    return m_positions.find(std::string(symbol)) != m_positions.end();
}

std::vector<TrackedPosition> PositionTracker::expired(std::chrono::system_clock::time_point now) const {
    std::lock_guard lock(m_mutex);
    std::vector<TrackedPosition> out;
    for (const auto& [_, pos] : m_positions) {
        if (pos.openingInFlight) {
            continue;
        }
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
        if (pos.openingInFlight) {
            continue;
        }
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

bool PositionTracker::applyExitFillByClientId(std::string_view clientOrderId, double filledDeltaQty) {
    if (filledDeltaQty <= 0.0) {
        return false;
    }

    std::lock_guard lock(m_mutex);
    for (auto it = m_positions.begin(); it != m_positions.end(); ++it) {
        auto& tracked = it->second;
        if (tracked.tpClientOrderId == clientOrderId || tracked.slClientOrderId == clientOrderId) {
            tracked.quantity = std::max(0.0, tracked.quantity - filledDeltaQty);
            if (tracked.quantity <= 1e-12) {
                m_positions.erase(it);
            }
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

bool PositionTracker::updateStopLoss(
    std::string_view symbol,
    int64_t slOrderId,
    std::string slClientOrderId,
    double currentTrailLevel) {
    std::lock_guard lock(m_mutex);
    const auto it = m_positions.find(std::string(symbol));
    if (it == m_positions.end()) {
        return false;
    }
    if (it->second.openingInFlight) {
        return false;
    }
    it->second.slOrderId = slOrderId;
    it->second.slClientOrderId = std::move(slClientOrderId);
    it->second.currentTrailLevel = currentTrailLevel;
    return true;
}

bool PositionTracker::updateTakeProfit(
    std::string_view symbol,
    int64_t tpOrderId,
    std::string tpClientOrderId) {
    std::lock_guard lock(m_mutex);
    const auto it = m_positions.find(std::string(symbol));
    if (it == m_positions.end()) {
        return false;
    }
    if (it->second.openingInFlight) {
        return false;
    }
    it->second.tpOrderId = tpOrderId;
    it->second.tpClientOrderId = std::move(tpClientOrderId);
    return true;
}

bool PositionTracker::clearTakeProfit(std::string_view symbol) {
    std::lock_guard lock(m_mutex);
    const auto it = m_positions.find(std::string(symbol));
    if (it == m_positions.end()) {
        return false;
    }
    if (it->second.openingInFlight) {
        return false;
    }
    it->second.tpOrderId = 0;
    it->second.tpClientOrderId.clear();
    return true;
}

bool PositionTracker::refreshPositionView(
    std::string_view symbol,
    double entryPrice,
    double quantity) {
    std::lock_guard lock(m_mutex);
    const auto it = m_positions.find(std::string(symbol));
    if (it == m_positions.end()) {
        return false;
    }
    if (it->second.openingInFlight) {
        return false;
    }
    it->second.entryPrice = entryPrice;
    it->second.quantity = std::max(0.0, quantity);
    return true;
}

bool PositionTracker::markRecoveredFromSnapshot(std::string_view symbol) {
    std::lock_guard lock(m_mutex);
    const auto it = m_positions.find(std::string(symbol));
    if (it == m_positions.end()) {
        return false;
    }
    if (it->second.openingInFlight) {
        return false;
    }
    it->second.recoveredFromSnapshot = true;
    return true;
}

} // namespace engine

