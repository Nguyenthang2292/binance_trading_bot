#include "backtest/parameter_space.h"
#include <cmath>
#include <algorithm>
#include <limits>

namespace backtest {

namespace {

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

    // Number of steps
    int numSteps = std::max(1, static_cast<int>(std::floor((range.max - range.min) / range.step)) + 1);
    
    for (int i = 0; i < numSteps; ++i) {
        double val = range.min + i * range.step;
        if (val > range.max) {
            val = range.max;
        }
        
        if (range.isInteger) {
            val = std::round(val);
        }
        
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

    const int numSteps = std::max(
        1,
        static_cast<int>(std::floor((range.max - range.min) / range.step)) + 1);

    int count = 0;
    for (int i = 0; i < numSteps && count < cap; ++i) {
        double val = range.min + i * range.step;
        if (val > range.max) {
            val = range.max;
        }
        if (range.isInteger) {
            val = std::round(val);
        }

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
                auto stepsOf = [](const ParamRange& r) {
                    if (r.step <= 0.0 || r.max <= r.min) return 1;
                    return static_cast<int>(std::floor((r.max - r.min) / r.step)) + 1;
                };
                return stepsOf(a) < stepsOf(b);
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
        if (newTotal >= totalCombos && totalCombos <= maxTotalCombos) {
            // Widening did not help; revert and stop.
            maxStepsIt->step = oldStep;
            break;
        }
        totalCombos = newTotal;
    }

    return totalCombos <= maxTotalCombos;
}

}  // namespace backtest
