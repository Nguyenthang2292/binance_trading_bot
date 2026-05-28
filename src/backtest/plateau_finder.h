/**
 * @file plateau_finder.h
 * @brief Plateau selection over scored parameter grid points.
 */

#pragma once

#include "backtest/parameter_space.h"

#include <optional>
#include <vector>

namespace backtest {

/**
 * @brief One scored parameter point after walk-forward evaluation.
 */
struct ScoredPoint {
    ParamPoint point;      ///< Parameter values.
    double oosSortino;     ///< Mean OOS sortino.
    double isSortino;      ///< Mean IS sortino.
    bool passedFilters;    ///< True when fold filters were satisfied.
};

/**
 * @brief Output of plateau detection.
 */
struct PlateauResult {
    ParamPoint center;                 ///< Center point snapped back to grid.
    std::vector<ParamPoint> survivors; ///< Plateau members passing filters.
    double centerSortinoIS{0.0};       ///< IS sortino at center (if evaluated).
    double centerSortinoOOS{0.0};      ///< OOS sortino at center (if evaluated).
};

/**
 * @brief Detects robust local plateaus near top-performing points.
 */
class PlateauFinder {
public:
    /**
     * @brief Finds plateau center and survivors from scored grid.
     */
    static std::optional<PlateauResult> find(
        const std::vector<ScoredPoint>& scoredGrid,
        const std::vector<ParamConstraint>& constraints,
        int neighborhoodRadius,
        int maxNeighborhoodSize,
        double minPassFraction);
};

}  // namespace backtest
