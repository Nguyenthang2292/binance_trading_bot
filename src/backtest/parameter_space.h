#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>

namespace backtest {

struct ParamRange {
    std::string name;
    double min{0.0};
    double max{0.0};
    double step{0.0};
    bool isInteger{false};
};

struct ParamConstraint {
    enum class Kind { LessThan, LessEqual };
    std::string left;
    Kind kind{Kind::LessThan};
    std::string right;
};

using ParamPoint = std::unordered_map<std::string, double>;

class ParameterSpace {
public:
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
    static bool evaluateConstraints(
        const ParamPoint& point,
        const std::vector<ParamConstraint>& constraints);

private:
    static int calculateTotalCombos(
        const std::vector<ParamRange>& ranges,
        const std::vector<ParamConstraint>& constraints);
};

}  // namespace backtest
