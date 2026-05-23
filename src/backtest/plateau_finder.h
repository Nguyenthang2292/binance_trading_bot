#pragma once

#include "backtest/parameter_space.h"

#include <optional>
#include <vector>

namespace backtest {

struct ScoredPoint { ParamPoint point; double oosSortino; double isSortino; bool passedFilters; };

struct PlateauResult {
    ParamPoint center;
    std::vector<ParamPoint> survivors;
    double centerSortinoIS{0.0};
    double centerSortinoOOS{0.0};
};

class PlateauFinder {
public:
    static std::optional<PlateauResult> find(
        const std::vector<ScoredPoint>& scoredGrid,
        const std::vector<ParamConstraint>& constraints,
        int neighborhoodRadius,
        int maxNeighborhoodSize,
        double minPassFraction);
};

}  // namespace backtest
