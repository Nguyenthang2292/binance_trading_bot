/**
 * @file parameter_space.h
 * @brief Parameter-range modeling, constraint checks, and grid budget control.
 */

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>

namespace backtest {

/**
 * @brief One tunable parameter range.
 */
struct ParamRange {
    std::string name;   ///< Parameter name.
    double min{0.0};    ///< Inclusive lower bound.
    double max{0.0};    ///< Inclusive upper bound.
    double step{0.0};   ///< Positive sampling step.
    bool isInteger{false};   ///< Treat values as integer lattice when true.
};

/**
 * @brief Ordering constraint between two parameter names.
 */
struct ParamConstraint {
    enum class Kind { LessThan, LessEqual };
    std::string left;   ///< Left parameter key.
    Kind kind{Kind::LessThan};  ///< Constraint operator.
    std::string right;  ///< Right parameter key.
};

/// @brief Concrete parameter point (`name -> value`).
using ParamPoint = std::unordered_map<std::string, double>;

/**
 * @brief Helper for generating and constraining parameter grids.
 */
class ParameterSpace {
public:
    /**
     * @brief Materializes the full constrained Cartesian grid.
     */
    static std::vector<ParamPoint> grid(
        const std::vector<ParamRange>& ranges,
        const std::vector<ParamConstraint>& constraints);

    // Returns true if the (possibly modified) ranges fit within maxTotalCombos
    // after the routine completes. Returns false if no amount of step-widening
    // can bring the grid under budget (e.g., every dim has only 1 value).
    static bool clampToBudget(
        std::vector<ParamRange>& ranges,
        const std::vector<ParamConstraint>& constraints,
        int maxTotalCombos);

    // Public for internal anonymous-namespace helpers; safe to call externally too.
    /**
     * @brief Evaluates ordering constraints on a (possibly partial) point.
     */
    static bool evaluateConstraints(
        const ParamPoint& point,
        const std::vector<ParamConstraint>& constraints);
};

}  // namespace backtest
