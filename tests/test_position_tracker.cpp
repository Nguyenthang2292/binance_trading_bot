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
    a.leverage = 25;
    Position b;
    b.symbol = "ETHUSDT";
    b.positionAmt = 0.0;
    tracker.loadFromSnapshot({a, b});

    EXPECT_TRUE(tracker.has("BTCUSDT"));
    EXPECT_FALSE(tracker.has("ETHUSDT"));
    const auto loaded = tracker.bySymbol("BTCUSDT");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->recoveredFromSnapshot);
    EXPECT_EQ(loaded->activeLeverage, 25);
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

TEST(PositionTrackerTest, UpdatesTakeProfitAndPositionView) {
    engine::PositionTracker tracker;

    engine::TrackedPosition tracked;
    tracked.symbol = "BTCUSDT";
    tracked.direction = strategy::Signal::Direction::Long;
    tracked.quantity = 0.5;
    tracked.entryPrice = 100.0;
    tracked.openedAt = std::chrono::system_clock::now();
    tracked.maxHoldDuration = std::chrono::hours(1);
    ASSERT_TRUE(tracker.add(tracked));

    EXPECT_TRUE(tracker.updateTakeProfit("BTCUSDT", 123, "tp-123"));
    auto updated = tracker.bySymbol("BTCUSDT");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->tpOrderId, 123);
    EXPECT_EQ(updated->tpClientOrderId, "tp-123");

    EXPECT_TRUE(tracker.refreshPositionView("BTCUSDT", 101.5, 0.75));
    updated = tracker.bySymbol("BTCUSDT");
    ASSERT_TRUE(updated.has_value());
    EXPECT_DOUBLE_EQ(updated->entryPrice, 101.5);
    EXPECT_DOUBLE_EQ(updated->quantity, 0.75);

    EXPECT_TRUE(tracker.refreshFromSnapshot("BTCUSDT", 102.0, 0.5, 12));
    updated = tracker.bySymbol("BTCUSDT");
    ASSERT_TRUE(updated.has_value());
    EXPECT_DOUBLE_EQ(updated->entryPrice, 102.0);
    EXPECT_DOUBLE_EQ(updated->quantity, 0.5);
    EXPECT_EQ(updated->activeLeverage, 12);

    EXPECT_TRUE(tracker.clearTakeProfit("BTCUSDT"));
    updated = tracker.bySymbol("BTCUSDT");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->tpOrderId, 0);
    EXPECT_TRUE(updated->tpClientOrderId.empty());
}

