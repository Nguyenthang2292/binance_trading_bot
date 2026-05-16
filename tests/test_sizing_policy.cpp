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

    EXPECT_DOUBLE_EQ(result.notional, 50.0);
    EXPECT_FALSE(result.isMinClamped);
    EXPECT_DOUBLE_EQ(result.quantity, 0.5);
}

TEST(SizingPolicyTest, RoundsQuantityUpWhenStepFloorWouldViolateMinNotional) {
    const auto result = engine::calculateSize(
        engine::SizingInput{
            .availableBalance = 100.0,
            .atr = 100.0,
            .riskPct = 0.01,
            .slMultiplier = 2.0,
            .minNotional = 5.0,
        },
        333.0,
        0.01);

    EXPECT_TRUE(result.isMinClamped);
    EXPECT_DOUBLE_EQ(result.quantity, 0.02);
    EXPECT_GE(result.notional, 5.0);
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

TEST(SizingPolicyTest, CapsNotionalToConfiguredMaximum) {
    const auto result = engine::calculateSize(
        engine::SizingInput{
            .availableBalance = 12.0,
            .atr = 0.001,
            .riskPct = 0.01,
            .slMultiplier = 1.5,
            .minNotional = 5.0,
            .maxNotional = 6.0,
        },
        1.0,
        0.1);

    EXPECT_TRUE(result.isMaxCapped);
    EXPECT_DOUBLE_EQ(result.notional, 6.0);
    EXPECT_DOUBLE_EQ(result.quantity, 6.0);
}

TEST(SizingPolicyTest, ReturnsZeroWhenMaximumIsBelowMinimum) {
    const auto result = engine::calculateSize(
        engine::SizingInput{
            .availableBalance = 8.0,
            .atr = 0.001,
            .riskPct = 0.01,
            .slMultiplier = 1.5,
            .minNotional = 5.0,
            .maxNotional = 4.0,
        },
        1.0,
        0.1);

    EXPECT_DOUBLE_EQ(result.notional, 0.0);
    EXPECT_DOUBLE_EQ(result.quantity, 0.0);
}

