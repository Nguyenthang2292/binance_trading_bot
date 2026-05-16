#include <gtest/gtest.h>

#include "engine/position_tracker.h"

TEST(PositionTrackerTest, AddRemoveHasAndExpired) {
    engine::PositionTracker tracker;

    engine::TrackedPosition pos;
    pos.symbol = "BTCUSDT";
    pos.direction = strategy::Signal::Direction::Long;
    pos.openedAt = std::chrono::system_clock::now() - std::chrono::minutes(20);
    pos.maxHoldDuration = std::chrono::minutes(10);
    pos.tpClientOrderId = "tp-1";
    pos.slClientOrderId = "sl-1";
    tracker.add(pos);

    EXPECT_TRUE(tracker.has("BTCUSDT"));
    const auto expired = tracker.expired(std::chrono::system_clock::now());
    ASSERT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired[0].symbol, "BTCUSDT");

    EXPECT_TRUE(tracker.removeByExitOrderClientId("tp-1"));
    EXPECT_FALSE(tracker.has("BTCUSDT"));

    tracker.add(pos);
    tracker.remove("BTCUSDT");
    EXPECT_FALSE(tracker.has("BTCUSDT"));
}

TEST(PositionTrackerTest, LoadFromSnapshotKeepsNonZeroPositions) {
    engine::PositionTracker tracker;
    Position a;
    a.symbol = "BTCUSDT";
    a.positionAmt = 0.01;
    a.entryPrice = 100.0;
    Position b;
    b.symbol = "ETHUSDT";
    b.positionAmt = 0.0;
    tracker.loadFromSnapshot({a, b});

    EXPECT_TRUE(tracker.has("BTCUSDT"));
    EXPECT_FALSE(tracker.has("ETHUSDT"));
}

TEST(PositionTrackerTest, ReserveCommitAndPartialFillBehavior) {
    engine::PositionTracker tracker;

    EXPECT_TRUE(tracker.reserve("BTCUSDT"));
    EXPECT_TRUE(tracker.has("BTCUSDT"));

    engine::TrackedPosition tracked;
    tracked.symbol = "BTCUSDT";
    tracked.direction = strategy::Signal::Direction::Long;
    tracked.quantity = 1.5;
    tracked.tpClientOrderId = "tp-x";
    tracked.openedAt = std::chrono::system_clock::now();
    tracked.maxHoldDuration = std::chrono::hours(1);
    EXPECT_TRUE(tracker.commitReserved("BTCUSDT", tracked));

    EXPECT_FALSE(tracker.reserve("BTCUSDT"));
    EXPECT_FALSE(tracker.add(tracked));

    EXPECT_TRUE(tracker.applyExitFillByClientId("tp-x", 0.4));
    const auto afterPartial = tracker.bySymbol("BTCUSDT");
    ASSERT_TRUE(afterPartial.has_value());
    EXPECT_NEAR(afterPartial->quantity, 1.1, 1e-12);

    EXPECT_TRUE(tracker.applyExitFillByClientId("tp-x", 1.1));
    EXPECT_FALSE(tracker.has("BTCUSDT"));
}

