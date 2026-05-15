#include "engine/trailing_stop_controller.h"

#include <algorithm>
#include <limits>

namespace engine {

std::optional<TrailingStopDecision> TrailingStopController::evaluate(
    const TrackedPosition& position,
    const scanner::KlineCache& cache) const {
    if (!position.trailingEnabled || position.trailingCandles <= 0 || position.trailingInterval.empty()) {
        return std::nullopt;
    }
    if (position.direction == strategy::Signal::Direction::None) {
        return std::nullopt;
    }

    const auto klines = cache.snapshot(position.symbol, position.trailingInterval);
    if (!klines || klines->empty()) {
        return std::nullopt;
    }

    double candidate = position.direction == strategy::Signal::Direction::Long
        ? std::numeric_limits<double>::max()
        : std::numeric_limits<double>::lowest();
    int closedCandles = 0;

    for (auto it = klines->rbegin(); it != klines->rend() && closedCandles < position.trailingCandles; ++it) {
        if (!it->isClosed) {
            continue;
        }
        if (position.direction == strategy::Signal::Direction::Long) {
            candidate = std::min(candidate, it->low);
        } else {
            candidate = std::max(candidate, it->high);
        }
        ++closedCandles;
    }

    if (closedCandles == 0 || candidate <= 0.0) {
        return std::nullopt;
    }
    if (!isFavorableMove(position, candidate)) {
        return std::nullopt;
    }

    return TrailingStopDecision{
        .newLevel = candidate,
        .reason = "trailing stop moved using last " + std::to_string(closedCandles) + " closed " +
            position.trailingInterval + " candles",
    };
}

bool TrailingStopController::isFavorableMove(const TrackedPosition& position, double newLevel) {
    if (newLevel <= 0.0) {
        return false;
    }
    if (position.currentTrailLevel <= 0.0) {
        return true;
    }
    if (position.direction == strategy::Signal::Direction::Long) {
        return newLevel > position.currentTrailLevel;
    }
    if (position.direction == strategy::Signal::Direction::Short) {
        return newLevel < position.currentTrailLevel;
    }
    return false;
}

} // namespace engine
