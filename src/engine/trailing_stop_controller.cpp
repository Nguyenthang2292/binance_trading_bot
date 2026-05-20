#include "engine/trailing_stop_controller.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

namespace engine {

std::optional<TrailingStopDecision> TrailingStopController::evaluate(
    const TrackedPosition& position,
    const scanner::KlineCache& cache) const {
    if (!position.trailingEnabled || position.trailingInterval.empty()) {
        return std::nullopt;
    }
    if (position.trailingPolicy == strategy::Signal::ExitPolicy::Default && position.trailingCandles <= 0) {
        return std::nullopt;
    }
    if (position.trailingPolicy == strategy::Signal::ExitPolicy::SwingTrailing && position.swingLookback <= 0) {
        return std::nullopt;
    }
    if (position.direction == strategy::Signal::Direction::None) {
        return std::nullopt;
    }

    const auto klines = cache.snapshot(position.symbol, position.trailingInterval);
    if (!klines || klines->empty()) {
        return std::nullopt;
    }

    std::vector<Kline> closedKlines;
    closedKlines.reserve(klines->size());
    for (const auto& kline : *klines) {
        if (kline.isClosed) {
            closedKlines.push_back(kline);
        }
    }
    if (closedKlines.empty()) {
        return std::nullopt;
    }

    if (position.trailingPolicy == strategy::Signal::ExitPolicy::SwingTrailing) {
        const int lookback = std::max(1, position.swingLookback);
        const std::optional<double> candidate = position.direction == strategy::Signal::Direction::Long
            ? latestConfirmedSwingLow(closedKlines, lookback)
            : latestConfirmedSwingHigh(closedKlines, lookback);
        if (!candidate.has_value() || *candidate <= 0.0) {
            return std::nullopt;
        }
        if (!isFavorableMove(position, *candidate)) {
            return std::nullopt;
        }

        return TrailingStopDecision{
            .newLevel = *candidate,
            .reason = "trailing stop moved using latest confirmed swing point lookback=" + std::to_string(lookback) +
                " tf=" + position.trailingInterval,
        };
    }

    double candidate = position.direction == strategy::Signal::Direction::Long
        ? std::numeric_limits<double>::max()
        : std::numeric_limits<double>::lowest();
    int inspectedClosedCandles = 0;

    for (auto it = closedKlines.rbegin(); it != closedKlines.rend() && inspectedClosedCandles < position.trailingCandles; ++it) {
        if (position.direction == strategy::Signal::Direction::Long) {
            candidate = std::min(candidate, it->low);
        } else {
            candidate = std::max(candidate, it->high);
        }
        ++inspectedClosedCandles;
    }

    if (inspectedClosedCandles == 0 || candidate <= 0.0 || !isFavorableMove(position, candidate)) {
        return std::nullopt;
    }

    return TrailingStopDecision{
        .newLevel = candidate,
        .reason = "trailing stop moved using last " + std::to_string(inspectedClosedCandles) + " closed " +
            position.trailingInterval + " candles",
    };
}

std::optional<double> TrailingStopController::latestConfirmedSwingLow(const std::vector<Kline>& closedKlines, int lookback) {
    if (lookback <= 0) {
        return std::nullopt;
    }
    if (closedKlines.size() < static_cast<std::size_t>(lookback * 2 + 1)) {
        return std::nullopt;
    }

    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(closedKlines.size()) - 1 - lookback; i >= lookback; --i) {
        bool isSwing = true;
        for (int offset = -lookback; offset <= lookback; ++offset) {
            if (offset == 0) {
                continue;
            }
            const auto idx = static_cast<std::size_t>(i + offset);
            if (closedKlines[idx].low <= closedKlines[static_cast<std::size_t>(i)].low) {
                isSwing = false;
                break;
            }
        }
        if (isSwing) {
            return closedKlines[static_cast<std::size_t>(i)].low;
        }
    }
    return std::nullopt;
}

std::optional<double> TrailingStopController::latestConfirmedSwingHigh(const std::vector<Kline>& closedKlines, int lookback) {
    if (lookback <= 0) {
        return std::nullopt;
    }
    if (closedKlines.size() < static_cast<std::size_t>(lookback * 2 + 1)) {
        return std::nullopt;
    }

    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(closedKlines.size()) - 1 - lookback; i >= lookback; --i) {
        bool isSwing = true;
        for (int offset = -lookback; offset <= lookback; ++offset) {
            if (offset == 0) {
                continue;
            }
            const auto idx = static_cast<std::size_t>(i + offset);
            if (closedKlines[idx].high >= closedKlines[static_cast<std::size_t>(i)].high) {
                isSwing = false;
                break;
            }
        }
        if (isSwing) {
            return closedKlines[static_cast<std::size_t>(i)].high;
        }
    }
    return std::nullopt;
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
