#include "backtest/parameter_space.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace backtest;

TEST(ParameterSpaceTest, GridSizeMatchesProduct) {
    std::vector<ParamRange> ranges = {
        {"a", 10.0, 20.0, 5.0, true},   // 3 values: 10,15,20
        {"b",  1.0,  3.0, 1.0, true},   // 3 values: 1,2,3
    };
    auto grid = ParameterSpace::grid(ranges, {});
    EXPECT_EQ(grid.size(), 9u);
}

TEST(ParameterSpaceTest, ConstraintsFilterCombos) {
    std::vector<ParamRange> ranges = {
        {"ma_short", 10.0, 20.0, 5.0, true},
        {"ma_long",  10.0, 20.0, 5.0, true},
    };
    std::vector<ParamConstraint> constraints = {
        {"ma_short", ParamConstraint::Kind::LessThan, "ma_long"}
    };
    auto grid = ParameterSpace::grid(ranges, constraints);
    // Pairs satisfying ma_short < ma_long: (10,15)(10,20)(15,20)
    ASSERT_EQ(grid.size(), 3u);
    for (const auto& p : grid) {
        EXPECT_LT(p.at("ma_short"), p.at("ma_long"));
    }
}

TEST(ParameterSpaceTest, IntegerSnapping) {
    std::vector<ParamRange> ranges = {
        {"x", 1.0, 3.0, 0.7, true}  // 0.7 step but isInteger -> snap
    };
    auto grid = ParameterSpace::grid(ranges, {});
    for (const auto& p : grid) {
        const double v = p.at("x");
        EXPECT_DOUBLE_EQ(v, std::round(v));
    }
}

TEST(ParameterSpaceTest, ClampToBudgetReducesGrid) {
    std::vector<ParamRange> ranges = {
        {"a", 0.0, 100.0, 1.0, true},   // 101 values
        {"b", 0.0, 100.0, 1.0, true},   // 101 values → 10201 combos
    };
    const bool fit = ParameterSpace::clampToBudget(ranges, {}, 5000);
    EXPECT_TRUE(fit);
    auto grid = ParameterSpace::grid(ranges, {});
    EXPECT_LE(grid.size(), 5000u);
}

TEST(ParameterSpaceTest, ClampToBudgetHandlesHugeRangesWithoutMaterializingGrid) {
    std::vector<ParamRange> ranges = {
        {"a", 0.0, 1'000'000.0, 1.0, true},
        {"b", 0.0, 1'000'000.0, 1.0, true},
    };

    const bool fit = ParameterSpace::clampToBudget(ranges, {}, 1000);

    ASSERT_TRUE(fit);
    auto grid = ParameterSpace::grid(ranges, {});
    EXPECT_LE(grid.size(), 1000u);
}

TEST(ParameterSpaceTest, ClampToBudgetReturnsFalseWhenUnreducible) {
    // Single point in each dim, total = 1, but maxTotalCombos = 0 → cannot fit.
    std::vector<ParamRange> ranges = {
        {"a", 5.0, 5.0, 1.0, true},
    };
    EXPECT_FALSE(ParameterSpace::clampToBudget(ranges, {}, 0));
}
