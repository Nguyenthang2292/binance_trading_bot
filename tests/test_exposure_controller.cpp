#include <gtest/gtest.h>

#include "engine/exposure_controller.h"

namespace {

engine::TrackedPosition tracked(
    std::string symbol,
    strategy::Signal::Direction direction,
    double quantity,
    double entryPrice) {
    engine::TrackedPosition pos;
    pos.symbol = std::move(symbol);
    pos.direction = direction;
    pos.quantity = quantity;
    pos.entryPrice = entryPrice;
    return pos;
}

} // namespace

TEST(ExposureControllerTest, AllowsWhenDeviationSmall) {
    scanner::KlineCache cache(64);
    engine::ExposureConfig cfg;
    cfg.enabled = true;
    cfg.defaultBeta = 1.0;
    cfg.softLimitNetBeta = 0.5;
    cfg.hardLimitNetBeta = 1.0;
    cfg.maxGrossBeta = 3.0;

    engine::ExposureController controller(cfg, cache);
    engine::PositionTracker tracker;
    account::AccountSnapshot snapshot;
    const auto result = controller.check(
        "ETHUSDT",
        strategy::Signal::Direction::Long,
        100.0,
        tracker,
        snapshot,
        1000.0);
    EXPECT_EQ(result.decision, engine::ExposureDecision::Allow);
}

TEST(ExposureControllerTest, ScalesDownInSoftZone) {
    scanner::KlineCache cache(64);
    engine::ExposureConfig cfg;
    cfg.enabled = true;
    cfg.defaultBeta = 1.0;
    cfg.softLimitNetBeta = 0.1;
    cfg.hardLimitNetBeta = 0.2;
    cfg.maxGrossBeta = 10.0;

    engine::ExposureController controller(cfg, cache);
    engine::PositionTracker tracker;
    account::AccountSnapshot snapshot;
    const auto result = controller.check(
        "ETHUSDT",
        strategy::Signal::Direction::Long,
        150.0,
        tracker,
        snapshot,
        1000.0);
    EXPECT_EQ(result.decision, engine::ExposureDecision::ScaleDown);
    EXPECT_NEAR(result.scaleFactor, 0.5, 1e-9);
}

TEST(ExposureControllerTest, BlocksAtHardLimit) {
    scanner::KlineCache cache(64);
    engine::ExposureConfig cfg;
    cfg.enabled = true;
    cfg.defaultBeta = 1.0;
    cfg.softLimitNetBeta = 0.1;
    cfg.hardLimitNetBeta = 0.2;
    cfg.maxGrossBeta = 10.0;

    engine::ExposureController controller(cfg, cache);
    engine::PositionTracker tracker;
    account::AccountSnapshot snapshot;
    const auto result = controller.check(
        "ETHUSDT",
        strategy::Signal::Direction::Long,
        220.0,
        tracker,
        snapshot,
        1000.0);
    EXPECT_EQ(result.decision, engine::ExposureDecision::Block);
}

TEST(ExposureControllerTest, BlocksGrossUsingAbsoluteBetaWeightedNotional) {
    scanner::KlineCache cache(64);
    engine::ExposureConfig cfg;
    cfg.enabled = true;
    cfg.defaultBeta = -1.0;
    cfg.softLimitNetBeta = 0.1;
    cfg.hardLimitNetBeta = 0.2;
    cfg.maxGrossBeta = 0.1;

    engine::ExposureController controller(cfg, cache);
    engine::PositionTracker tracker;
    account::AccountSnapshot snapshot;
    const auto result = controller.check(
        "XRPUSDT",
        strategy::Signal::Direction::Long,
        150.0,
        tracker,
        snapshot,
        1000.0);
    EXPECT_EQ(result.decision, engine::ExposureDecision::Block);
}

TEST(ExposureControllerTest, AllowsWhenTradeImprovesDeviation) {
    scanner::KlineCache cache(64);
    engine::ExposureConfig cfg;
    cfg.enabled = true;
    cfg.defaultBeta = 1.0;
    cfg.softLimitNetBeta = 0.5;
    cfg.hardLimitNetBeta = 1.0;
    cfg.maxGrossBeta = 10.0;

    engine::ExposureController controller(cfg, cache);
    engine::PositionTracker tracker;
    tracker.add(tracked("ADAUSDT", strategy::Signal::Direction::Long, 6.0, 100.0)); // 600 net long beta
    account::AccountSnapshot snapshot;
    const auto result = controller.check(
        "BTCUSDT",
        strategy::Signal::Direction::Short,
        100.0,
        tracker,
        snapshot,
        1000.0);
    EXPECT_EQ(result.decision, engine::ExposureDecision::Allow);
}

TEST(ExposureControllerTest, HardLimitBlocksEvenIfDeviationImproves) {
    scanner::KlineCache cache(64);
    engine::ExposureConfig cfg;
    cfg.enabled = true;
    cfg.defaultBeta = 1.0;
    cfg.targetNetBeta = 0.0;
    cfg.softLimitNetBeta = 0.8;
    cfg.hardLimitNetBeta = 1.0;
    cfg.maxGrossBeta = 10.0;

    engine::ExposureController controller(cfg, cache);
    engine::PositionTracker tracker;
    tracker.add(tracked("BTCUSDT", strategy::Signal::Direction::Long, 15.0, 100.0)); // current net=1500
    account::AccountSnapshot snapshot;
    const auto result = controller.check(
        "ETHUSDT",
        strategy::Signal::Direction::Short,
        400.0,
        tracker,
        snapshot,
        1000.0);
    EXPECT_EQ(result.decision, engine::ExposureDecision::Block);
}
