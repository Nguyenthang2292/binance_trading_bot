#include "backtest/parameter_space.h"
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <limits>

namespace backtest {

namespace {

int clampStepCount(long double rawSteps, int cap = std::numeric_limits<int>::max()) {
    if (!std::isfinite(rawSteps) || rawSteps <= 0.0L) {
        return 0;
    }
    const long double capped = std::min<long double>(rawSteps, static_cast<long double>(cap));
    return std::max(1, static_cast<int>(std::floor(capped + 1e-12L)));
}

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

int countGridRecursiveCapped(
    const std::vector<ParamRange>& ranges,
    const std::vector<ParamConstraint>& constraints,
    size_t currentIndex,
    ParamPoint& currentPoint,
    int cap) {

    if (cap <= 0) {
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

int ParameterSpace::calculateTotalCombos(
    const std::vector<ParamRange>& ranges,
    const std::vector<ParamConstraint>& constraints) {
    ParamPoint currentPoint;
    return countGridRecursiveCapped(
        ranges,
        constraints,
        0,
        currentPoint,
        std::numeric_limits<int>::max());
}

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
