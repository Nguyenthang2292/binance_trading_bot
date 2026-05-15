#include <gtest/gtest.h>

#include "engine/sizing_policy.h"

TEST(SizingPolicyTest, AppliesFormulaAndStepRounding) {
    const auto result = engine::calculateSize(
        engine::SizingInput{
            .availableBalance = 1000.0,
            .atr = 10.0,
            .riskPct = 0.01,
            .slMultiplier = 2.0,
            .minNotional = 1.0,
        },
        100.0,
        0.001);

    EXPECT_DOUBLE_EQ(result.notional, 1.0);
    EXPECT_TRUE(result.isMinClamped);
    EXPECT_DOUBLE_EQ(result.quantity, 0.01);
}

TEST(SizingPolicyTest, ZeroAtrReturnsZero) {
    const auto result = engine::calculateSize(
        engine::SizingInput{
            .availableBalance = 1000.0,
            .atr = 0.0,
            .riskPct = 0.01,
            .slMultiplier = 2.0,
            .minNotional = 1.0,
        },
        100.0,
        0.001);
    EXPECT_DOUBLE_EQ(result.notional, 0.0);
    EXPECT_DOUBLE_EQ(result.quantity, 0.0);
}

