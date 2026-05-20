#include <gtest/gtest.h>

#include "engine/trailing_stop_controller.h"

namespace {

void push(scanner::KlineCache& cache, double high, double low, bool closed = true) {
    static int64_t openTime = 1000;
    Kline k;
    k.openTime = openTime++;
    k.high = high;
    k.low = low;
    k.close = (high + low) / 2.0;
    k.isClosed = closed;
    cache.update("BTCUSDT", "4h", k);
}

engine::TrackedPosition longPosition(double currentTrailLevel) {
    engine::TrackedPosition pos;
    pos.symbol = "BTCUSDT";
    pos.direction = strategy::Signal::Direction::Long;
    pos.trailingEnabled = true;
    pos.trailingInterval = "4h";
    pos.trailingCandles = 3;
    pos.currentTrailLevel = currentTrailLevel;
    return pos;
}

engine::TrackedPosition shortPosition(double currentTrailLevel) {
    engine::TrackedPosition pos;
    pos.symbol = "BTCUSDT";
    pos.direction = strategy::Signal::Direction::Short;
    pos.trailingEnabled = true;
    pos.trailingInterval = "4h";
    pos.trailingCandles = 3;
    pos.currentTrailLevel = currentTrailLevel;
    return pos;
}

} // namespace

TEST(TrailingStopControllerTest, MovesLongStopUsingClosedCandlesOnly) {
    scanner::KlineCache cache(10);
    push(cache, 120.0, 101.0);
    push(cache, 121.0, 102.0);
    push(cache, 122.0, 103.0);
    push(cache, 200.0, 50.0, false);

    const auto decision = engine::TrailingStopController{}.evaluate(longPosition(90.0), cache);

    ASSERT_TRUE(decision.has_value());
    EXPECT_DOUBLE_EQ(decision->newLevel, 101.0);
}

TEST(TrailingStopControllerTest, SkipsUnfavorableLongMove) {
    scanner::KlineCache cache(10);
    push(cache, 120.0, 80.0);
    push(cache, 121.0, 81.0);
    push(cache, 122.0, 82.0);

    const auto decision = engine::TrailingStopController{}.evaluate(longPosition(90.0), cache);

    EXPECT_FALSE(decision.has_value());
}

TEST(TrailingStopControllerTest, MovesLongStopUsingLatestConfirmedSwingLow) {
    scanner::KlineCache cache(10);
    push(cache, 120.0, 100.0);
    push(cache, 121.0, 95.0);
    push(cache, 122.0, 101.0);
    push(cache, 123.0, 96.0);
    push(cache, 124.0, 102.0);
    push(cache, 200.0, 50.0, false);

    auto pos = longPosition(90.0);
    pos.trailingPolicy = strategy::Signal::ExitPolicy::SwingTrailing;
    pos.swingLookback = 1;

    const auto decision = engine::TrailingStopController{}.evaluate(pos, cache);

    ASSERT_TRUE(decision.has_value());
    EXPECT_DOUBLE_EQ(decision->newLevel, 96.0);
}

TEST(TrailingStopControllerTest, MovesShortStopUsingLatestConfirmedSwingHigh) {
    scanner::KlineCache cache(10);
    push(cache, 100.0, 80.0);
    push(cache, 105.0, 81.0);
    push(cache, 101.0, 82.0);
    push(cache, 104.0, 83.0);
    push(cache, 99.0, 84.0);
    push(cache, 200.0, 10.0, false);

    auto pos = shortPosition(110.0);
    pos.trailingPolicy = strategy::Signal::ExitPolicy::SwingTrailing;
    pos.swingLookback = 1;

    const auto decision = engine::TrailingStopController{}.evaluate(pos, cache);

    ASSERT_TRUE(decision.has_value());
    EXPECT_DOUBLE_EQ(decision->newLevel, 104.0);
}
