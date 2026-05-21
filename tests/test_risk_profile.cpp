#include "engine/risk_profile.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace engine;
using nlohmann::json;

TEST(RiskProfileTest, MissingActiveProfileDisablesOverride) {
    json root = {
        {"engine", {{"max_position_notional_x_available_balance", 0.8}}}
    };

    auto result = RiskProfileLoadResult::loadActive(root);
    EXPECT_FALSE(result.enabled);
    EXPECT_TRUE(result.warnings.empty());
}

TEST(RiskProfileTest, UnknownProfileFallsBackToModerateWithWarning) {
    json root = {
        {"active_profile", "unknown_profile"},
        {"risk_profiles", {
            {"moderate", {{"risk_pct", 0.01}}}
        }}
    };

    auto result = RiskProfileLoadResult::loadActive(root);
    EXPECT_TRUE(result.enabled);
    EXPECT_EQ(result.profile.name, "moderate");
    EXPECT_DOUBLE_EQ(result.profile.riskPct, 0.01);
    EXPECT_FALSE(result.warnings.empty());
    EXPECT_NE(result.warnings[0].find("unknown or missing active risk profile"), std::string::npos);
}

TEST(RiskProfileTest, LoadsValidProfile) {
    json root = {
        {"active_profile", "aggressive"},
        {"risk_profiles", {
            {"aggressive", {
                {"risk_pct", 0.05},
                {"sl_multiplier", 1.2},
                {"max_position_notional_x_available_balance", 0.75},
                {"max_total_notional_pct", 15.0},
                {"soft_max_drawdown", 0.30},
                {"hard_max_drawdown", 0.65},
                {"soft_min_upi", 0.2},
                {"hard_min_upi", -2.0},
                {"soft_limit_net_beta", 0.8},
                {"hard_limit_net_beta", 1.5},
                {"max_gross_beta", 5.0}
            }}
        }}
    };

    auto result = RiskProfileLoadResult::loadActive(root);
    EXPECT_TRUE(result.enabled);
    EXPECT_TRUE(result.warnings.empty());
    
    EXPECT_EQ(result.profile.name, "aggressive");
    EXPECT_DOUBLE_EQ(result.profile.riskPct, 0.05);
    EXPECT_DOUBLE_EQ(result.profile.slMultiplier, 1.2);
    EXPECT_DOUBLE_EQ(result.profile.maxPositionNotionalXAvailableBalance, 0.75);
    EXPECT_DOUBLE_EQ(result.profile.maxTotalNotionalPct, 15.0);
    EXPECT_DOUBLE_EQ(result.profile.softMaxDrawdown, 0.30);
    EXPECT_DOUBLE_EQ(result.profile.hardMaxDrawdown, 0.65);
    EXPECT_DOUBLE_EQ(result.profile.softMinUpi, 0.2);
    EXPECT_DOUBLE_EQ(result.profile.hardMinUpi, -2.0);
    EXPECT_DOUBLE_EQ(result.profile.softLimitNetBeta, 0.8);
    EXPECT_DOUBLE_EQ(result.profile.hardLimitNetBeta, 1.5);
    EXPECT_DOUBLE_EQ(result.profile.maxGrossBeta, 5.0);
}

TEST(RiskProfileTest, MissingFieldsUseModerateDefaults) {
    json root = {
        {"active_profile", "custom"},
        {"risk_profiles", {
            {"custom", {
                {"risk_pct", 0.02} // Only providing one field
            }}
        }}
    };

    auto result = RiskProfileLoadResult::loadActive(root);
    EXPECT_TRUE(result.enabled);
    EXPECT_TRUE(result.warnings.empty());
    
    EXPECT_EQ(result.profile.name, "custom");
    EXPECT_DOUBLE_EQ(result.profile.riskPct, 0.02);
    // Defaults from moderate:
    EXPECT_DOUBLE_EQ(result.profile.slMultiplier, 1.5);
    EXPECT_DOUBLE_EQ(result.profile.softMaxDrawdown, 0.20);
}

TEST(RiskProfileTest, InvalidProfileFallsBackToModerateWithWarning) {
    json root = {
        {"active_profile", "bad_profile"},
        {"risk_profiles", {
            {"bad_profile", {
                {"risk_pct", 1.5} // Invalid, must be < 1
            }}
        }}
    };

    auto result = RiskProfileLoadResult::loadActive(root);
    EXPECT_TRUE(result.enabled);
    EXPECT_EQ(result.profile.name, "moderate");
    EXPECT_DOUBLE_EQ(result.profile.riskPct, 0.01); // Fallback to moderate
    EXPECT_FALSE(result.warnings.empty());
    EXPECT_NE(result.warnings[0].find("invalid risk profile"), std::string::npos);
}

TEST(RiskProfileTest, ValidationCrossFields) {
    json root = {
        {"active_profile", "bad_profile"},
        {"risk_profiles", {
            {"bad_profile", {
                {"soft_max_drawdown", 0.40},
                {"hard_max_drawdown", 0.20} // Invalid: soft > hard
            }}
        }}
    };

    auto result = RiskProfileLoadResult::loadActive(root);
    EXPECT_TRUE(result.enabled);
    EXPECT_EQ(result.profile.name, "moderate"); // Fallback
    EXPECT_FALSE(result.warnings.empty());
    EXPECT_NE(result.warnings[0].find("invalid risk profile"), std::string::npos);
}

TEST(RiskProfileTest, ActiveProfilePresentButRiskProfilesBlockAbsent) {
    // active_profile is set but the risk_profiles block is missing entirely
    json root = {
        {"active_profile", "conservative"}
    };

    auto result = RiskProfileLoadResult::loadActive(root);
    EXPECT_TRUE(result.enabled);
    EXPECT_EQ(result.profile.name, "moderate"); // Fallback to moderate
    EXPECT_DOUBLE_EQ(result.profile.riskPct, 0.01);
    EXPECT_FALSE(result.warnings.empty());
    EXPECT_NE(result.warnings[0].find("unknown or missing active risk profile"), std::string::npos);
}

TEST(RiskProfileTest, ActiveProfileIsNotAString) {
    // active_profile set to a non-string value
    json root = {
        {"active_profile", 42}
    };

    auto result = RiskProfileLoadResult::loadActive(root);
    EXPECT_TRUE(result.enabled);
    EXPECT_EQ(result.profile.name, "moderate"); // Fallback to moderate
    EXPECT_FALSE(result.warnings.empty());
    EXPECT_NE(result.warnings[0].find("active_profile must be a string"), std::string::npos);
}
