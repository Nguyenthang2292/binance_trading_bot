/**
 * @file parameter_space.cpp
 * @brief Parameter grid generation and combo-budget clamping logic.
 */

#include "backtest/parameter_space.h"
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <limits>

namespace backtest {

namespace {

/**
 * @brief Clamp a raw step count to a finite positive integer.
 *
 * @param rawSteps The raw number of candidate values.
 * @param cap Upper bound applied before converting to an integer.
 * @return A clamped step count, or 0 if the input is not usable.
 */
int clampStepCount(long double rawSteps, int cap = std::numeric_limits<int>::max()) {
    if (!std::isfinite(rawSteps) || rawSteps <= 0.0L) {
        return 0;
    }
    const long double capped = std::min<long double>(rawSteps, static_cast<long double>(cap));
    return std::max(1, static_cast<int>(std::floor(capped + 1e-12L)));
}

/**
 * @brief Count the number of discrete values in a range.
 *
 * Integer ranges are normalized to whole-number boundaries before counting.
 * Floating-point ranges use the configured step size directly.
 *
 * @param range Parameter range to inspect.
 * @param cap Upper bound applied to the result.
 * @return Number of discrete values reachable from the range.
 */
int discreteStepCount(const ParamRange& range, int cap = std::numeric_limits<int>::max()) {
    if (range.step <= 0.0 || range.max < range.min) {
        return 0;
    }

    if (range.isInteger) {
        const long double minValue = std::ceil(static_cast<long double>(range.min) - 1e-9L);
        const long double maxValue = std::floor(static_cast<long double>(range.max) + 1e-9L);
        if (maxValue < minValue) {
            return 0;
        }
        const long double intStep = std::max<long double>(1.0L, std::round(static_cast<long double>(range.step)));
        const long double rawSteps = std::floor((maxValue - minValue) / intStep) + 1.0L;
        return clampStepCount(rawSteps, cap);
    }

    const long double span = static_cast<long double>(range.max) - static_cast<long double>(range.min);
    const long double rawSteps = std::floor(span / static_cast<long double>(range.step) + 1e-12L) + 1.0L;
    return clampStepCount(rawSteps, cap);
}

/**
 * @brief Materialize the value at a specific step index.
 *
 * @param range Parameter range to sample.
 * @param index Zero-based step index.
 * @return The parameter value represented by the requested step.
 */
double valueForStep(const ParamRange& range, int index) {
    if (range.isInteger) {
        const long double start = std::ceil(static_cast<long double>(range.min) - 1e-9L);
        const long double end = std::floor(static_cast<long double>(range.max) + 1e-9L);
        const long double intStep = std::max<long double>(1.0L, std::round(static_cast<long double>(range.step)));
        long double value = start + static_cast<long double>(index) * intStep;
        value = std::min(value, end);
        return static_cast<double>(std::llround(value));
    }

    double value = range.min + static_cast<double>(index) * range.step;
    if (value > range.max) {
        value = range.max;
    }
    return value;
}

/**
 * @brief Recursively build the full parameter grid.
 *
 * The recursion assigns one parameter at a time and only keeps points that
 * satisfy all constraints once every range has been assigned.
 *
 * @param ranges Parameter ranges to expand.
 * @param constraints Constraint list that must hold for each point.
 * @param currentIndex Index of the range currently being expanded.
 * @param currentPoint Partially built parameter point.
 * @param result Output collection for valid parameter combinations.
 */
void buildGridRecursive(
    const std::vector<ParamRange>& ranges,
    const std::vector<ParamConstraint>& constraints,
    size_t currentIndex,
    ParamPoint& currentPoint,
    std::vector<ParamPoint>& result) {
    
    if (currentIndex >= ranges.size()) {
        if (ParameterSpace::evaluateConstraints(currentPoint, constraints)) {
            result.push_back(currentPoint);
        }
        return;
    }

    const auto& range = ranges[currentIndex];
    
    // Safety check for infinite loops or invalid steps
    if (range.step <= 0.0 || range.max < range.min) {
        return; 
    }

    const int numSteps = discreteStepCount(range);
    
    for (int i = 0; i < numSteps; ++i) {
        const double val = valueForStep(range, i);
        currentPoint[range.name] = val;
        buildGridRecursive(ranges, constraints, currentIndex + 1, currentPoint, result);
    }
}

/**
 * @brief Count valid grid combinations while respecting a hard cap.
 *
 * @param ranges Parameter ranges to count.
 * @param constraints Constraint list applied to each candidate point.
 * @param currentIndex Index of the range currently being expanded.
 * @param currentPoint Partially built parameter point.
 * @param cap Maximum number of combinations to count.
 * @return Number of valid combinations, capped at @p cap.
 */
int countGridRecursiveCapped(
    const std::vector<ParamRange>& ranges,
    const std::vector<ParamConstraint>& constraints,
    size_t currentIndex,
    ParamPoint& currentPoint,
    int cap) {

    if (cap <= 0) {
/**
 * @brief Expand a set of parameter ranges into the full valid grid.
 *
 * @param ranges Parameter ranges to expand.
 * @param constraints Constraint list that filters the generated points.
 * @return All parameter points that satisfy the supplied constraints.
 */
        return 0;
    }

    if (currentIndex >= ranges.size()) {
        return ParameterSpace::evaluateConstraints(currentPoint, constraints) ? 1 : 0;
    }

    const auto& range = ranges[currentIndex];
    if (range.step <= 0.0 || range.max < range.min) {
        return 0;
    }

    const int numSteps = discreteStepCount(range, cap);

    int count = 0;
    for (int i = 0; i < numSteps && count < cap; ++i) {
        const double val = valueForStep(range, i);

        currentPoint[range.name] = val;
        if (ParameterSpace::evaluateConstraints(currentPoint, constraints)) {
            count += countGridRecursiveCapped(
                ranges,
                constraints,
                currentIndex + 1,
                currentPoint,
                cap - count);
        }
    }
    currentPoint.erase(range.name);
    return count;
}

} // namespace

std::vector<ParamPoint> ParameterSpace::grid(
    const std::vector<ParamRange>& ranges,
    const std::vector<ParamConstraint>& constraints) {
    
    std::vector<ParamPoint> result;
    ParamPoint currentPoint;
    buildGridRecursive(ranges, constraints, 0, currentPoint, result);
    return result;
}

/**
 * @brief Check whether a parameter point satisfies all constraints.
 *
 * Constraints are skipped until both referenced parameters are available in
 * the point, which lets the recursive grid builder evaluate them incrementally.
 *
 * @param point Candidate parameter point.
 * @param constraints Constraint list to evaluate.
 * @return true when all fully-specified constraints pass.
 */
bool ParameterSpace::evaluateConstraints(
    const ParamPoint& point,
    const std::vector<ParamConstraint>& constraints) {
    
    for (const auto& c : constraints) {
        auto itLeft = point.find(c.left);
        auto itRight = point.find(c.right);
        
        // If either param isn't in the point yet, we can't fully evaluate. 
        // We assume valid until both are present.
        if (itLeft == point.end() || itRight == point.end()) {
            continue; 
        }

        double leftVal = itLeft->second;
        double rightVal = itRight->second;

        if (c.kind == ParamConstraint::Kind::LessThan) {
            if (!(leftVal < rightVal)) return false;
        } else if (c.kind == ParamConstraint::Kind::LessEqual) {
            if (!(leftVal <= rightVal)) return false;
        }
    }
    return true;
}

/**
 * @brief Reduce the parameter grid until it fits a combination budget.
 *
 * The function widens the step size of the densest range until the estimated
 * number of combinations is within the requested budget or no further reduction
 * is possible.
 *
 * @param ranges Parameter ranges that may be modified in place.
 * @param constraints Constraint list applied during counting.
 * @param maxTotalCombos Maximum number of combinations allowed.
 * @return true if the ranges fit within the requested budget after clamping.
 */
bool ParameterSpace::clampToBudget(
    std::vector<ParamRange>& ranges,
    const std::vector<ParamConstraint>& constraints,
    int maxTotalCombos) {

    if (ranges.empty() || maxTotalCombos <= 0) return ranges.empty();

    ParamPoint countPoint;
    const int countCap = maxTotalCombos == std::numeric_limits<int>::max()
        ? maxTotalCombos
        : maxTotalCombos + 1;
    int totalCombos = countGridRecursiveCapped(ranges, constraints, 0, countPoint, countCap);

    // Iteratively widen the step of the range with most discrete values until
    // total combos fits the budget OR no further reduction is possible.
    int safetyIter = 0;
    while (totalCombos > maxTotalCombos && safetyIter++ < 64) {
        auto maxStepsIt = std::max_element(
            ranges.begin(), ranges.end(),
            [](const ParamRange& a, const ParamRange& b) {
                return discreteStepCount(a) < discreteStepCount(b);
            });

        if (maxStepsIt == ranges.end()) break;
        if (maxStepsIt->step <= 0.0 || maxStepsIt->max <= maxStepsIt->min) break;

        const double oldStep = maxStepsIt->step;
        double newStep = oldStep * 2.0;
        if (maxStepsIt->isInteger) {
            newStep = std::max(oldStep + 1.0, std::round(newStep));
        }
        maxStepsIt->step = newStep;

        ParamPoint nextCountPoint;
        const int newTotal = countGridRecursiveCapped(
            ranges,
            constraints,
            0,
            nextCountPoint,
            countCap);
        const bool countWasSaturated = totalCombos >= countCap;
        if (newTotal >= totalCombos && !countWasSaturated) {
            // Widening did not help; revert and stop.
            maxStepsIt->step = oldStep;
            break;
        }
        totalCombos = newTotal;
    }

    return totalCombos <= maxTotalCombos;
}

}  // namespace backtest
