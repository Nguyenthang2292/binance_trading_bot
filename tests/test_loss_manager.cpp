#include <gtest/gtest.h>

#include "engine/loss_manager.h"
#include "engine/signal_engine.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <optional>
#include <utility>

namespace {

template <typename T>
T runAwaitable(boost::asio::io_context& ioc, boost::asio::awaitable<T> task) {
    auto fut = boost::asio::co_spawn(ioc, std::move(task), boost::asio::use_future);
    ioc.run();
    return fut.get();
}

class MockOrdersPort final : public engine::IOrdersPort {
public:
    int openNormalOrdersCalls{0};
    int marketCalls{0};
    int limitCalls{0};
    int amendLimitCalls{0};
    int protectionCalls{0};
    int cancelAlgoByAlgoIdCalls{0};
    std::optional<MarketOrderDraft> lastMarketDraft;
    std::optional<LimitOrderDraft> lastLimitDraft;
    std::optional<AmendLimitOrderDraft> lastAmendDraft;
    std::optional<ProtectionOrderDraft> lastProtectionDraft;

    OrdersResult<NormalPlacementResult> marketResult = [] {
        NormalPlacementResult out;
        out.state = PlacementState::Accepted;
        out.orderId = 101;
        out.clientOrderId = "dca-101";
        return OrdersResult<NormalPlacementResult>(out);
    }();

    OrdersResult<NormalPlacementResult> limitResult = [] {
        NormalPlacementResult out;
        out.state = PlacementState::Accepted;
        out.orderId = 202;
        out.clientOrderId = "tp-202";
        return OrdersResult<NormalPlacementResult>(out);
    }();

    OrdersResult<NormalOrderSnapshot> amendResult = [] {
        NormalOrderSnapshot out;
        out.symbol = "BTCUSDT";
        out.orderId = 303;
        out.clientOrderId = "tp-303";
        out.status = "NEW";
        return OrdersResult<NormalOrderSnapshot>(out);
    }();

    OrdersResult<NormalPlacementResult> protectionResult = [] {
        NormalPlacementResult out;
        out.state = PlacementState::Accepted;
        out.orderId = 404;
        out.clientOrderId = "sl-404";
        return OrdersResult<NormalPlacementResult>(out);
    }();

    OrdersResult<std::vector<NormalOrderSnapshot>> openNormalOrdersResult =
        std::vector<NormalOrderSnapshot>{};

    boost::asio::awaitable<OrdersResult<std::vector<NormalOrderSnapshot>>>
    openNormalOrders(std::optional<Symbol>) override {
        ++openNormalOrdersCalls;
        co_return openNormalOrdersResult;
    }

    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> market(MarketOrderDraft draft) override {
        ++marketCalls;
        lastMarketDraft = std::move(draft);
        co_return marketResult;
    }

    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> limit(LimitOrderDraft draft) override {
        ++limitCalls;
        lastLimitDraft = std::move(draft);
        co_return limitResult;
    }

    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> amendLimitOrder(AmendLimitOrderDraft draft) override {
        ++amendLimitCalls;
        lastAmendDraft = std::move(draft);
        co_return amendResult;
    }

    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> protection(ProtectionOrderDraft draft) override {
        ++protectionCalls;
        lastProtectionDraft = std::move(draft);
        co_return protectionResult;
    }

    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> closeByMarket(CloseByMarketDraft) override {
        co_return NormalPlacementResult{};
    }

    boost::asio::awaitable<OrdersResult<LeverageResult>> setLeverage(Symbol, int) override {
        co_return LeverageResult{};
    }

    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelNormalByOrderId(Symbol, int64_t) override {
        co_return NormalCancelResult{};
    }

    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelNormalByClientOrderId(Symbol, ClientOrderId) override {
        co_return NormalCancelResult{};
    }

    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelAlgoByAlgoId(Symbol, int64_t) override {
        ++cancelAlgoByAlgoIdCalls;
        co_return NormalCancelResult{};
    }

    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelAlgoByClientAlgoId(Symbol, ClientAlgoId) override {
        co_return NormalCancelResult{};
    }
};

class MockOrderCapPort final : public engine::IOrderCapPort {
public:
    engine::OrderCapResult next{
        .decision = engine::OrderCapDecision::Allow,
        .reason = "ok",
    };

    engine::OrderCapResult check(
        double,
        const account::AccountSnapshot&,
        const engine::PositionTracker&) const override {
        return next;
    }

    engine::OrderCapFailureMode failureMode() const override {
        return engine::OrderCapFailureMode::Open;
    }
};

class MockExposurePort final : public engine::IExposurePort {
public:
    engine::ExposureCheckResult next{
        .decision = engine::ExposureDecision::Allow,
        .scaleFactor = 1.0,
        .reason = "ok",
    };

    engine::ExposureCheckResult check(
        std::string_view,
        strategy::Signal::Direction,
        double,
        const engine::PositionTracker&,
        const account::AccountSnapshot&,
        double) const override {
        return next;
    }

    engine::ExposureMetrics currentMetrics(
        const engine::PositionTracker&,
        const account::AccountSnapshot&,
        double) const override {
        return {};
    }

    engine::ExposureFailureMode failureMode() const override {
        return engine::ExposureFailureMode::Open;
    }

    double minNotionalAfterScale() const override {
        return 0.0;
    }
};

account::AccountSnapshot singlePositionSnapshot(const Position& pos) {
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    snapshot.positions = std::vector<Position>{pos};
    return snapshot;
}

std::optional<ExchangeSymbol> symbolInfo(std::string_view symbol) {
    ExchangeSymbol out;
    out.symbol = std::string(symbol);
    out.stepSize = 0.001;
    out.tickSize = 0.1;
    return out;
}

} // namespace

TEST(LossManagerTest, ValidateConfigRejectsInvalidThresholds) {
    engine::LossManagerConfig cfg;
    cfg.enabled = true;
    cfg.roiBeThreshold = 0.0;
    std::string reason;
    EXPECT_FALSE(engine::LossManager::validateConfig(cfg, &reason));
    EXPECT_FALSE(reason.empty());
}

TEST(LossManagerTest, PlacesBreakEvenTakeProfitWhenThresholdHit) {
    boost::asio::io_context ioc;

    engine::PositionTracker tracker;
    engine::TrackedPosition tracked;
    tracked.symbol = "BTCUSDT";
    tracked.direction = strategy::Signal::Direction::Long;
    tracked.quantity = 0.01;
    tracked.entryPrice = 100.0;
    tracked.slOrderId = 505;
    tracked.slClientOrderId = "sl-existing";
    tracked.openedAt = std::chrono::system_clock::now();
    tracked.maxHoldDuration = std::chrono::hours(24);
    ASSERT_TRUE(tracker.add(tracked));

    Position live;
    live.symbol = "BTCUSDT";
    live.positionSide = PositionSide::Both;
    live.positionAmt = 0.01;
    live.entryPrice = 100.0;
    live.breakEvenPrice = 100.2;
    live.markPrice = 95.0;
    live.leverage = 10;

    MockOrdersPort orders;
    MockOrderCapPort orderCap;
    MockExposurePort exposure;
    engine::LossManagerConfig cfg;
    cfg.enabled = true;
    cfg.maxDcaCount = 0;
    engine::LossManager manager(cfg, orders, orderCap, exposure, tracker, symbolInfo);

    auto snapshot = singlePositionSnapshot(live);
    runAwaitable(ioc, manager.evaluate(snapshot, snapshot.account.availableBalance));

    EXPECT_EQ(orders.limitCalls, 1);
    EXPECT_EQ(orders.amendLimitCalls, 0);
    EXPECT_EQ(orders.marketCalls, 0);
    ASSERT_TRUE(orders.lastLimitDraft.has_value());
    EXPECT_EQ(orders.lastLimitDraft->side, OrderSide::Sell);
    ASSERT_TRUE(orders.lastLimitDraft->reduceOnly.has_value());
    EXPECT_TRUE(*orders.lastLimitDraft->reduceOnly);

    const auto updated = tracker.bySymbol("BTCUSDT");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->tpOrderId, 202);
    EXPECT_EQ(updated->tpClientOrderId, "tp-202");
}

TEST(LossManagerTest, AmendsExistingTakeProfitWhenThresholdHit) {
    boost::asio::io_context ioc;

    engine::PositionTracker tracker;
    engine::TrackedPosition tracked;
    tracked.symbol = "BTCUSDT";
    tracked.direction = strategy::Signal::Direction::Long;
    tracked.quantity = 0.01;
    tracked.entryPrice = 100.0;
    tracked.tpOrderId = 999;
    tracked.tpClientOrderId = "tp-existing";
    tracked.openedAt = std::chrono::system_clock::now();
    tracked.maxHoldDuration = std::chrono::hours(24);
    ASSERT_TRUE(tracker.add(tracked));

    Position live;
    live.symbol = "BTCUSDT";
    live.positionSide = PositionSide::Both;
    live.positionAmt = 0.01;
    live.entryPrice = 100.0;
    live.breakEvenPrice = 100.2;
    live.markPrice = 95.0;
    live.leverage = 10;

    MockOrdersPort orders;
    MockOrderCapPort orderCap;
    MockExposurePort exposure;
    engine::LossManagerConfig cfg;
    cfg.enabled = true;
    cfg.maxDcaCount = 0;
    engine::LossManager manager(cfg, orders, orderCap, exposure, tracker, symbolInfo);

    auto snapshot = singlePositionSnapshot(live);
    runAwaitable(ioc, manager.evaluate(snapshot, snapshot.account.availableBalance));

    EXPECT_EQ(orders.amendLimitCalls, 1);
    EXPECT_EQ(orders.limitCalls, 0);
    const auto updated = tracker.bySymbol("BTCUSDT");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->tpOrderId, 303);
    EXPECT_EQ(updated->tpClientOrderId, "tp-303");
}

TEST(LossManagerTest, FallsBackToPlaceWhenAmendOrderNotFound) {
    boost::asio::io_context ioc;

    engine::PositionTracker tracker;
    engine::TrackedPosition tracked;
    tracked.symbol = "BTCUSDT";
    tracked.direction = strategy::Signal::Direction::Long;
    tracked.quantity = 0.01;
    tracked.entryPrice = 100.0;
    tracked.tpOrderId = 999;
    tracked.tpClientOrderId = "tp-existing";
    tracked.openedAt = std::chrono::system_clock::now();
    tracked.maxHoldDuration = std::chrono::hours(24);
    ASSERT_TRUE(tracker.add(tracked));

    Position live;
    live.symbol = "BTCUSDT";
    live.positionSide = PositionSide::Both;
    live.positionAmt = 0.01;
    live.entryPrice = 100.0;
    live.breakEvenPrice = 100.2;
    live.markPrice = 95.0;
    live.leverage = 10;

    MockOrdersPort orders;
    orders.amendResult = std::unexpected(BinanceError::fromApiResponse(-2013, "Order does not exist"));
    MockOrderCapPort orderCap;
    MockExposurePort exposure;
    engine::LossManagerConfig cfg;
    cfg.enabled = true;
    cfg.maxDcaCount = 0;
    engine::LossManager manager(cfg, orders, orderCap, exposure, tracker, symbolInfo);

    auto snapshot = singlePositionSnapshot(live);
    runAwaitable(ioc, manager.evaluate(snapshot, snapshot.account.availableBalance));

    EXPECT_EQ(orders.amendLimitCalls, 1);
    EXPECT_EQ(orders.limitCalls, 1);
    const auto updated = tracker.bySymbol("BTCUSDT");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->tpOrderId, 202);
    EXPECT_EQ(updated->tpClientOrderId, "tp-202");
}

TEST(LossManagerTest, DcaPendingSkipsBreakEvenUntilPositionAmountIncreases) {
    boost::asio::io_context ioc;

    engine::PositionTracker tracker;
    engine::TrackedPosition tracked;
    tracked.symbol = "BTCUSDT";
    tracked.direction = strategy::Signal::Direction::Long;
    tracked.quantity = 0.01;
    tracked.entryPrice = 100.0;
    tracked.slOrderId = 505;
    tracked.slClientOrderId = "sl-existing";
    tracked.currentTrailLevel = 85.0;
    tracked.openedAt = std::chrono::system_clock::now();
    tracked.maxHoldDuration = std::chrono::hours(24);
    ASSERT_TRUE(tracker.add(tracked));

    MockOrdersPort orders;
    MockOrderCapPort orderCap;
    MockExposurePort exposure;
    engine::LossManagerConfig cfg;
    cfg.enabled = true;
    cfg.roiBeThreshold = -0.50;
    cfg.roiDcaThreshold = -0.80;
    cfg.maxDcaCount = 1;
    engine::LossManager manager(cfg, orders, orderCap, exposure, tracker, symbolInfo);

    Position before;
    before.symbol = "BTCUSDT";
    before.positionSide = PositionSide::Both;
    before.positionAmt = 0.01;
    before.entryPrice = 100.0;
    before.breakEvenPrice = 100.1;
    before.markPrice = 92.0;
    before.leverage = 10;

    auto pendingSnapshot = singlePositionSnapshot(before);
    runAwaitable(ioc, manager.evaluate(pendingSnapshot, pendingSnapshot.account.availableBalance));
    EXPECT_EQ(orders.marketCalls, 1);
    EXPECT_EQ(orders.limitCalls, 0);

    ioc.restart();
    runAwaitable(ioc, manager.evaluate(pendingSnapshot, pendingSnapshot.account.availableBalance));
    EXPECT_EQ(orders.marketCalls, 1);
    EXPECT_EQ(orders.limitCalls, 0);

    Position after = before;
    after.positionAmt = 0.02;
    after.entryPrice = 96.0;
    after.breakEvenPrice = 96.2;
    after.markPrice = 90.0;
    auto finalizedSnapshot = singlePositionSnapshot(after);
    ioc.restart();
    runAwaitable(ioc, manager.evaluate(finalizedSnapshot, finalizedSnapshot.account.availableBalance));

    EXPECT_EQ(orders.marketCalls, 1);
    EXPECT_EQ(orders.protectionCalls, 1);
    EXPECT_EQ(orders.cancelAlgoByAlgoIdCalls, 1);
    ASSERT_TRUE(orders.lastProtectionDraft.has_value());
    EXPECT_EQ(orders.lastProtectionDraft->closeSide, OrderSide::Sell);
    EXPECT_EQ(orders.lastProtectionDraft->triggerPrice.value(), "81");
    EXPECT_EQ(orders.limitCalls + orders.amendLimitCalls, 1);
    const auto updated = tracker.bySymbol("BTCUSDT");
    ASSERT_TRUE(updated.has_value());
    EXPECT_NEAR(updated->quantity, 0.02, 1e-12);
    EXPECT_NEAR(updated->entryPrice, 96.0, 1e-12);
    EXPECT_EQ(updated->slOrderId, 404);
    EXPECT_EQ(updated->slClientOrderId, "sl-404");
    EXPECT_NEAR(updated->currentTrailLevel, 81.0, 1e-12);
}

TEST(LossManagerTest, DcaSkippedWhenTrackedStopLossIsMissing) {
    boost::asio::io_context ioc;

    engine::PositionTracker tracker;
    engine::TrackedPosition tracked;
    tracked.symbol = "BTCUSDT";
    tracked.direction = strategy::Signal::Direction::Long;
    tracked.quantity = 0.01;
    tracked.entryPrice = 100.0;
    tracked.openedAt = std::chrono::system_clock::now();
    tracked.maxHoldDuration = std::chrono::hours(24);
    ASSERT_TRUE(tracker.add(tracked));

    Position live;
    live.symbol = "BTCUSDT";
    live.positionSide = PositionSide::Both;
    live.positionAmt = 0.01;
    live.entryPrice = 100.0;
    live.breakEvenPrice = 100.1;
    live.markPrice = 92.0;
    live.leverage = 10;

    MockOrdersPort orders;
    MockOrderCapPort orderCap;
    MockExposurePort exposure;
    engine::LossManagerConfig cfg;
    cfg.enabled = true;
    cfg.roiBeThreshold = -0.50;
    cfg.roiDcaThreshold = -0.80;
    cfg.maxDcaCount = 1;
    engine::LossManager manager(cfg, orders, orderCap, exposure, tracker, symbolInfo);

    auto snapshot = singlePositionSnapshot(live);
    runAwaitable(ioc, manager.evaluate(snapshot, snapshot.account.availableBalance));

    EXPECT_EQ(orders.marketCalls, 0);
    EXPECT_EQ(orders.protectionCalls, 0);
}

TEST(LossManagerTest, RecoveredPositionsDoNotDcaByDefault) {
    boost::asio::io_context ioc;

    Position recovered;
    recovered.symbol = "BTCUSDT";
    recovered.positionSide = PositionSide::Both;
    recovered.positionAmt = 0.01;
    recovered.entryPrice = 100.0;
    recovered.breakEvenPrice = 100.1;
    recovered.markPrice = 92.0;
    recovered.leverage = 10;

    engine::PositionTracker tracker;
    tracker.loadFromSnapshot({recovered});

    MockOrdersPort orders;
    MockOrderCapPort orderCap;
    MockExposurePort exposure;
    engine::LossManagerConfig cfg;
    cfg.enabled = true;
    cfg.maxDcaCount = 2;
    cfg.allowDcaOnRecoveredPositions = false;
    engine::LossManager manager(cfg, orders, orderCap, exposure, tracker, symbolInfo);

    auto snapshot = singlePositionSnapshot(recovered);
    runAwaitable(ioc, manager.evaluate(snapshot, snapshot.account.availableBalance));

    EXPECT_EQ(orders.marketCalls, 0);
    EXPECT_EQ(orders.limitCalls, 1);
}
