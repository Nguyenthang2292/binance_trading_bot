#include <gtest/gtest.h>

#include "strategy/strategy_config.h"

namespace {

strategy::StrategyConfig makeValidConfig() {
    strategy::StrategyConfig cfg;
    cfg.name = "demo";
    cfg.type = "demo_type";
    cfg.intervals = {"15m"};
    cfg.scanInterval = std::chrono::seconds{60};
    cfg.maxHoldDuration = std::chrono::seconds{3600};
    cfg.riskPct = 0.01;
    cfg.slMultiplier = 1.5;
    cfg.tpMultiplier = 3.0;
    cfg.takeProfitPercent = 10.0;
    cfg.leverage = 10;
    cfg.minNotional = 5.0;
    cfg.atrPeriod = 14;
    cfg.minConfidence = 0.2;
    return cfg;
}

}  // namespace

TEST(StrategyConfigTest, ValidConfigPassesValidation) {
    auto cfg = makeValidConfig();
    const auto result = strategy::validateStrategyConfig(cfg);
    EXPECT_TRUE(result.has_value());
}

TEST(StrategyConfigTest, RejectsOutOfRangeRiskPct) {
    auto cfg = makeValidConfig();
    cfg.riskPct = 1.5;
    const auto result = strategy::validateStrategyConfig(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("riskPct"), std::string::npos);
}

TEST(StrategyConfigTest, RejectsNonPositiveDurations) {
    auto cfg = makeValidConfig();
    cfg.scanInterval = std::chrono::seconds::zero();
    const auto scanResult = strategy::validateStrategyConfig(cfg);
    ASSERT_FALSE(scanResult.has_value());
    EXPECT_NE(scanResult.error().find("scanInterval"), std::string::npos);

    cfg = makeValidConfig();
    cfg.maxHoldDuration = std::chrono::seconds::zero();
    const auto holdResult = strategy::validateStrategyConfig(cfg);
    ASSERT_FALSE(holdResult.has_value());
    EXPECT_NE(holdResult.error().find("maxHoldDuration"), std::string::npos);
}

TEST(StrategyConfigTest, RejectsInvalidTrailingConfigWhenEnabled) {
    auto cfg = makeValidConfig();
    cfg.trailingStop.enabled = true;
    cfg.trailingStop.candles = 0;
    cfg.trailingStop.checkInterval = std::chrono::seconds{30};

    const auto candlesResult = strategy::validateStrategyConfig(cfg);
    ASSERT_FALSE(candlesResult.has_value());
    EXPECT_NE(candlesResult.error().find("trailingStop.candles"), std::string::npos);

    cfg.trailingStop.candles = 2;
    cfg.trailingStop.checkInterval = std::chrono::seconds::zero();
    const auto intervalResult = strategy::validateStrategyConfig(cfg);
    ASSERT_FALSE(intervalResult.has_value());
    EXPECT_NE(intervalResult.error().find("trailingStop.checkInterval"), std::string::npos);
}

TEST(StrategyConfigTest, RejectsInvalidAggregateRiskCaps) {
    auto cfg = makeValidConfig();
    cfg.maxConcurrentPositions = 0;
    const auto posResult = strategy::validateStrategyConfig(cfg);
    ASSERT_FALSE(posResult.has_value());
    EXPECT_NE(posResult.error().find("maxConcurrentPositions"), std::string::npos);

    cfg = makeValidConfig();
    cfg.maxTotalRiskPct = 1.2;
    const auto riskResult = strategy::validateStrategyConfig(cfg);
    ASSERT_FALSE(riskResult.has_value());
    EXPECT_NE(riskResult.error().find("maxTotalRiskPct"), std::string::npos);
}
