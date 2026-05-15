#pragma once

#include "engine/position_tracker.h"
#include "scanner/kline_cache.h"

#include <optional>
#include <string>

namespace engine {

struct TrailingStopDecision {
    double newLevel{0.0};
    std::string reason;
};

class TrailingStopController {
public:
    std::optional<TrailingStopDecision> evaluate(
        const TrackedPosition& position,
        const scanner::KlineCache& cache) const;

private:
    static bool isFavorableMove(const TrackedPosition& position, double newLevel);
};

} // namespace engine
