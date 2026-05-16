#include <gtest/gtest.h>

#include "engine/order_cap_controller.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

account::AccountSnapshot makeSnapshot(double totalMarginBalance, const std::vector<double>& notionals) {
    account::AccountSnapshot snapshot;
    snapshot.account.totalMarginBalance = totalMarginBalance;
    int idx = 0;
    for (const double notional : notionals) {
        Position pos;
        pos.symbol = "SYM" + std::to_string(idx++);
        pos.positionAmt = notional < 0.0 ? -1.0 : 1.0;
        pos.notional = notional;
        snapshot.account.positions.push_back(pos);
    }
    return snapshot;
}

} // namespace

TEST(OrderCapControllerTest, AllowsWhenWithinCap) {
    engine::OrderCapConfig cfg;
    cfg.enabled = true;
    cfg.maxTotalNotionalPct = 0.5;
    engine::TotalNotionalGuard guard(cfg);
    const auto snapshot = makeSnapshot(20.0, {4.0, 4.0});
    engine::PositionTracker tracker;

    const auto result = guard.check(2.0, snapshot, tracker);
    EXPECT_EQ(result.decision, engine::OrderCapDecision::Allow);
}

TEST(OrderCapControllerTest, AllowsAtCapBoundary) {
    engine::OrderCapConfig cfg;
    cfg.enabled = true;
    cfg.maxTotalNotionalPct = 0.5;
    engine::TotalNotionalGuard guard(cfg);
    const auto snapshot = makeSnapshot(20.0, {4.0, 4.0});
    engine::PositionTracker tracker;

    const auto result = guard.check(2.0, snapshot, tracker);
    EXPECT_EQ(result.decision, engine::OrderCapDecision::Allow);
    EXPECT_DOUBLE_EQ(result.cap, 10.0);
    EXPECT_DOUBLE_EQ(result.totalOpenNotional + result.proposedNotional, 10.0);
}

TEST(OrderCapControllerTest, BlocksWhenExceedCap) {
    engine::OrderCapConfig cfg;
    cfg.enabled = true;
    cfg.maxTotalNotionalPct = 0.5;
    engine::TotalNotionalGuard guard(cfg);
    const auto snapshot = makeSnapshot(20.0, {5.0, 4.0});
    engine::PositionTracker tracker;

    const auto result = guard.check(2.0, snapshot, tracker);
    EXPECT_EQ(result.decision, engine::OrderCapDecision::Block);
}

TEST(OrderCapControllerTest, BlocksWhenMarginBalanceIsZero) {
    engine::OrderCapConfig cfg;
    cfg.enabled = true;
    cfg.maxTotalNotionalPct = 0.5;
    engine::TotalNotionalGuard guard(cfg);
    const auto snapshot = makeSnapshot(0.0, {});
    engine::PositionTracker tracker;

    const auto result = guard.check(1.0, snapshot, tracker);
    EXPECT_EQ(result.decision, engine::OrderCapDecision::Block);
}

TEST(OrderCapControllerTest, UsesAbsForShortNotional) {
    engine::OrderCapConfig cfg;
    cfg.enabled = true;
    cfg.maxTotalNotionalPct = 0.5;
    engine::TotalNotionalGuard guard(cfg);
    const auto snapshot = makeSnapshot(20.0, {-5.0});
    engine::PositionTracker tracker;

    const auto result = guard.check(2.0, snapshot, tracker);
    EXPECT_EQ(result.decision, engine::OrderCapDecision::Allow);
    EXPECT_DOUBLE_EQ(result.totalOpenNotional, 5.0);
}

TEST(OrderCapControllerTest, MergesTrackerWhenRemotePositionMissing) {
    engine::OrderCapConfig cfg;
    cfg.enabled = true;
    cfg.maxTotalNotionalPct = 0.5;
    engine::TotalNotionalGuard guard(cfg);
    const auto snapshot = makeSnapshot(20.0, {});
    engine::PositionTracker tracker;
    engine::TrackedPosition pos;
    pos.symbol = "BTCUSDT";
    pos.quantity = 0.1;
    pos.entryPrice = 50.0;
    tracker.add(pos);

    const auto result = guard.check(4.0, snapshot, tracker);
    EXPECT_EQ(result.decision, engine::OrderCapDecision::Allow);
    EXPECT_DOUBLE_EQ(result.totalOpenNotional, 5.0);
}

TEST(OrderCapControllerTest, UsesMaxBetweenRemoteAndTrackerForSameSymbol) {
    // Real one-way mode scenario: Binance returns positionSide=Both for all positions.
    // Tracker has direction=Long. Both refer to the same physical position → take max, not sum.
    engine::OrderCapConfig cfg;
    cfg.enabled = true;
    cfg.maxTotalNotionalPct = 0.5;
    engine::TotalNotionalGuard guard(cfg);
    auto snapshot = makeSnapshot(30.0, {});
    Position pos;
    pos.symbol = "BTCUSDT";
    pos.positionSide = PositionSide::Both;  // Binance one-way always returns Both
    pos.positionAmt = 1.0;
    pos.notional = 5.0;
    snapshot.account.positions.push_back(pos);

    engine::PositionTracker tracker;
    engine::TrackedPosition tracked;
    tracked.symbol = "BTCUSDT";
    tracked.direction = strategy::Signal::Direction::Long;  // real bot sets Long/Short
    tracked.quantity = 0.2;
    tracked.entryPrice = 50.0;  // tracker notional = 10
    tracker.add(tracked);

    const auto result = guard.check(0.1, snapshot, tracker);
    EXPECT_EQ(result.decision, engine::OrderCapDecision::Allow);
    EXPECT_DOUBLE_EQ(result.totalOpenNotional, 10.0);  // max(5, 10) = 10, not 5+10=15
}

TEST(OrderCapControllerTest, NoOpAlwaysAllows) {
    engine::NoOpOrderCapPort noOp;
    const auto snapshot = makeSnapshot(0.0, {1000.0});
    engine::PositionTracker tracker;

    const auto result = noOp.check(999.0, snapshot, tracker);
    EXPECT_EQ(result.decision, engine::OrderCapDecision::Allow);
    EXPECT_EQ(noOp.failureMode(), engine::OrderCapFailureMode::Open);
}

TEST(OrderCapControllerTest, ThrowsWhenMaxTotalNotionalPctIsNonPositive) {
    account::AccountSnapshot snapshot;
    snapshot.account.totalMarginBalance = 100.0;
    engine::PositionTracker tracker;

    engine::OrderCapConfig zeroCfg;
    zeroCfg.enabled = true;
    zeroCfg.maxTotalNotionalPct = 0.0;
    engine::TotalNotionalGuard zeroGuard(zeroCfg);
    EXPECT_THROW((void)zeroGuard.check(1.0, snapshot, tracker), std::invalid_argument);

    engine::OrderCapConfig negativeCfg;
    negativeCfg.enabled = true;
    negativeCfg.maxTotalNotionalPct = -0.1;
    engine::TotalNotionalGuard negativeGuard(negativeCfg);
    EXPECT_THROW((void)negativeGuard.check(1.0, snapshot, tracker), std::invalid_argument);
}
