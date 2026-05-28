#include <gtest/gtest.h>

#include "engine/signal_engine.h"
#include "engine/take_profit_reconciler.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

template <typename T>
T runAwaitable(boost::asio::io_context& ioc, boost::asio::awaitable<T> task) {
    auto fut = boost::asio::co_spawn(ioc, std::move(task), boost::asio::use_future);
    ioc.restart();
    ioc.run();
    return fut.get();
}

class OrdersStub final : public engine::IOrdersPort {
public:
    int openNormalOrdersCalls{0};
    int limitCalls{0};
    int cancelNormalByOrderIdCalls{0};
    int cancelNormalByClientOrderIdCalls{0};
    std::optional<LimitOrderDraft> lastLimitDraft;

    OrdersResult<std::vector<NormalOrderSnapshot>> openOrdersResult = std::vector<NormalOrderSnapshot>{};
    OrdersResult<NormalPlacementResult> limitResult = [] {
        NormalPlacementResult out;
        out.state = PlacementState::Accepted;
        out.orderId = 101;
        return OrdersResult<NormalPlacementResult>(out);
    }();
    OrdersResult<NormalCancelResult> cancelResult = NormalCancelResult{};

    boost::asio::awaitable<OrdersResult<std::vector<NormalOrderSnapshot>>>
    openNormalOrders(std::optional<Symbol>) override {
        ++openNormalOrdersCalls;
        co_return openOrdersResult;
    }

    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> market(MarketOrderDraft) override {
        co_return NormalPlacementResult{};
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> limit(LimitOrderDraft draft) override {
        ++limitCalls;
        lastLimitDraft = std::move(draft);
        co_return limitResult;
    }
    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> amendLimitOrder(AmendLimitOrderDraft) override {
        co_return NormalOrderSnapshot{};
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> protection(ProtectionOrderDraft) override {
        co_return NormalPlacementResult{};
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> closeByMarket(CloseByMarketDraft) override {
        co_return NormalPlacementResult{};
    }
    boost::asio::awaitable<OrdersResult<LeverageResult>> setLeverage(Symbol, int) override {
        co_return LeverageResult{};
    }
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelNormalByOrderId(Symbol, int64_t) override {
        ++cancelNormalByOrderIdCalls;
        co_return cancelResult;
    }
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelNormalByClientOrderId(
        Symbol,
        ClientOrderId) override {
        ++cancelNormalByClientOrderIdCalls;
        co_return cancelResult;
    }
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelAlgoByAlgoId(Symbol, int64_t) override {
        co_return NormalCancelResult{};
    }
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelAlgoByClientAlgoId(Symbol, ClientAlgoId) override {
        co_return NormalCancelResult{};
    }
};

account::AccountSnapshot snapshotWithPositions(std::vector<Position> positions) {
    account::AccountSnapshot snapshot;
    snapshot.positions = std::move(positions);
    return snapshot;
}

Position longPosition() {
    Position p;
    p.symbol = "BTCUSDT";
    p.positionAmt = 0.01;
    p.entryPrice = 100.0;
    p.leverage = 10;
    return p;
}

Position shortPosition() {
    Position p;
    p.symbol = "BTCUSDT";
    p.positionAmt = -0.01;
    p.entryPrice = 100.03;
    p.leverage = 10;
    return p;
}

std::optional<ExchangeSymbol> symbolInfo(std::string_view symbol) {
    ExchangeSymbol out;
    out.symbol = std::string(symbol);
    out.stepSize = 0.001;
    out.tickSize = 0.1;
    return out;
}

} // namespace

TEST(TakeProfitReconcilerTest, PlacesGlobalTakeProfitWhenNoCoverageExists) {
    boost::asio::io_context ioc;
    OrdersStub orders;
    engine::PositionTracker tracker;
    engine::TakeProfitReconcilerConfig cfg;
    cfg.enabled = true;
    cfg.maxOrdersPerCycle = 8;
    engine::TakeProfitReconciler reconciler(cfg, orders, tracker, symbolInfo, 5.0, std::chrono::hours(24));

    runAwaitable(ioc, reconciler.reconcileOnce(snapshotWithPositions({longPosition()})));

    EXPECT_EQ(orders.openNormalOrdersCalls, 1);
    EXPECT_EQ(orders.limitCalls, 1);
    ASSERT_TRUE(orders.lastLimitDraft.has_value());
    EXPECT_EQ(orders.lastLimitDraft->symbol, "BTCUSDT");
    EXPECT_EQ(orders.lastLimitDraft->side, OrderSide::Sell);
    ASSERT_TRUE(orders.lastLimitDraft->reduceOnly.has_value());
    EXPECT_TRUE(*orders.lastLimitDraft->reduceOnly);
    EXPECT_EQ(std::string(orders.lastLimitDraft->price.value()), "100.5");
    EXPECT_EQ(std::string(orders.lastLimitDraft->quantity.value()), "0.01");
    ASSERT_TRUE(orders.lastLimitDraft->clientOrderId.has_value());
    EXPECT_TRUE(orders.lastLimitDraft->clientOrderId->starts_with("gtp_"));

    const auto tracked = tracker.bySymbol("BTCUSDT");
    ASSERT_TRUE(tracked.has_value());
    EXPECT_EQ(tracked->tpOrderId, 101);
    EXPECT_TRUE(tracked->tpClientOrderId.starts_with("gtp_"));
}

TEST(TakeProfitReconcilerTest, AdoptsSelfOwnedTakeProfitWithoutPlacingNewOrder) {
    boost::asio::io_context ioc;
    OrdersStub orders;
    NormalOrderSnapshot existing;
    existing.symbol = "BTCUSDT";
    existing.orderId = 222;
    existing.clientOrderId = "gtp_BTCUSDT_abc_00";
    existing.side = OrderSide::Sell;
    existing.type = OrderType::Limit;
    existing.status = "NEW";
    existing.origQty = "0.01";
    existing.executedQty = "0";
    existing.reduceOnly = true;
    orders.openOrdersResult = std::vector<NormalOrderSnapshot>{existing};

    engine::PositionTracker tracker;
    engine::TrackedPosition tracked;
    tracked.symbol = "BTCUSDT";
    tracked.direction = strategy::Signal::Direction::Long;
    tracked.entryPrice = 100.0;
    tracked.quantity = 0.01;
    tracked.openedAt = std::chrono::system_clock::now();
    tracked.maxHoldDuration = std::chrono::hours(24);
    ASSERT_TRUE(tracker.add(tracked));

    engine::TakeProfitReconcilerConfig cfg;
    cfg.enabled = true;
    engine::TakeProfitReconciler reconciler(cfg, orders, tracker, symbolInfo, 5.0, std::chrono::hours(24));

    runAwaitable(ioc, reconciler.reconcileOnce(snapshotWithPositions({longPosition()})));

    EXPECT_EQ(orders.limitCalls, 0);
    const auto updated = tracker.bySymbol("BTCUSDT");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->tpOrderId, 222);
    EXPECT_EQ(updated->tpClientOrderId, "gtp_BTCUSDT_abc_00");
    EXPECT_DOUBLE_EQ(updated->entryPrice, 100.0);
    EXPECT_DOUBLE_EQ(updated->quantity, 0.01);
    EXPECT_EQ(updated->activeLeverage, 10);
}

TEST(TakeProfitReconcilerTest, ExternalCoverageSkipsPlacementAndDoesNotAdoptTrackerTp) {
    boost::asio::io_context ioc;
    OrdersStub orders;
    NormalOrderSnapshot manual;
    manual.symbol = "BTCUSDT";
    manual.orderId = 333;
    manual.clientOrderId = "manual_tp_1";
    manual.side = OrderSide::Sell;
    manual.type = OrderType::Limit;
    manual.status = "NEW";
    manual.origQty = "0.01";
    manual.executedQty = "0";
    manual.reduceOnly = true;
    orders.openOrdersResult = std::vector<NormalOrderSnapshot>{manual};

    engine::PositionTracker tracker;
    engine::TrackedPosition tracked;
    tracked.symbol = "BTCUSDT";
    tracked.direction = strategy::Signal::Direction::Long;
    tracked.entryPrice = 100.0;
    tracked.quantity = 0.01;
    tracked.openedAt = std::chrono::system_clock::now();
    tracked.maxHoldDuration = std::chrono::hours(24);
    ASSERT_TRUE(tracker.add(tracked));

    engine::TakeProfitReconcilerConfig cfg;
    cfg.enabled = true;
    engine::TakeProfitReconciler reconciler(cfg, orders, tracker, symbolInfo, 5.0, std::chrono::hours(24));

    runAwaitable(ioc, reconciler.reconcileOnce(snapshotWithPositions({longPosition()})));

    EXPECT_EQ(orders.limitCalls, 0);
    const auto updated = tracker.bySymbol("BTCUSDT");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->tpOrderId, 0);
    EXPECT_TRUE(updated->tpClientOrderId.empty());
}

TEST(TakeProfitReconcilerTest, CancelsStaleSelfOwnedOrdersForFlatSymbols) {
    boost::asio::io_context ioc;
    OrdersStub orders;
    NormalOrderSnapshot stale;
    stale.symbol = "BTCUSDT";
    stale.orderId = 444;
    stale.clientOrderId = "gtp_BTCUSDT_stale_00";
    stale.side = OrderSide::Sell;
    stale.type = OrderType::Limit;
    stale.status = "NEW";
    stale.origQty = "0.01";
    stale.executedQty = "0";
    stale.reduceOnly = true;

    NormalOrderSnapshot manual;
    manual.symbol = "ETHUSDT";
    manual.orderId = 555;
    manual.clientOrderId = "manual_tp_1";
    manual.side = OrderSide::Sell;
    manual.type = OrderType::Limit;
    manual.status = "NEW";
    manual.origQty = "0.02";
    manual.executedQty = "0";
    manual.reduceOnly = true;

    orders.openOrdersResult = std::vector<NormalOrderSnapshot>{stale, manual};

    engine::PositionTracker tracker;
    engine::TakeProfitReconcilerConfig cfg;
    cfg.enabled = true;
    cfg.maxOrdersPerCycle = 2;
    engine::TakeProfitReconciler reconciler(cfg, orders, tracker, symbolInfo, 5.0, std::chrono::hours(24));

    runAwaitable(ioc, reconciler.reconcileOnce(snapshotWithPositions({})));

    EXPECT_EQ(orders.cancelNormalByOrderIdCalls, 1);
    EXPECT_EQ(orders.cancelNormalByClientOrderIdCalls, 0);
    EXPECT_EQ(orders.limitCalls, 0);
}

TEST(TakeProfitReconcilerTest, SkipsFreshSelfOwnedStaleCancelWithinGraceWindow) {
    boost::asio::io_context ioc;
    OrdersStub orders;
    NormalOrderSnapshot fresh;
    fresh.symbol = "BTCUSDT";
    fresh.orderId = 444;
    fresh.clientOrderId = "gtp_BTCUSDT_stale_00";
    fresh.side = OrderSide::Sell;
    fresh.type = OrderType::Limit;
    fresh.status = "NEW";
    fresh.origQty = "0.01";
    fresh.executedQty = "0";
    fresh.reduceOnly = true;
    fresh.updateTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    orders.openOrdersResult = std::vector<NormalOrderSnapshot>{fresh};

    engine::PositionTracker tracker;
    engine::TakeProfitReconcilerConfig cfg;
    cfg.enabled = true;
    engine::TakeProfitReconciler reconciler(
        cfg,
        orders,
        tracker,
        symbolInfo,
        5.0,
        std::chrono::hours(24),
        std::chrono::seconds(60));

    runAwaitable(ioc, reconciler.reconcileOnce(snapshotWithPositions({})));

    EXPECT_EQ(orders.cancelNormalByOrderIdCalls, 0);
    EXPECT_EQ(orders.cancelNormalByClientOrderIdCalls, 0);
    EXPECT_EQ(orders.limitCalls, 0);
}

TEST(TakeProfitReconcilerTest, PlacesGlobalTakeProfitForShortPositionWithRoundUp) {
    boost::asio::io_context ioc;
    OrdersStub orders;
    engine::PositionTracker tracker;
    engine::TakeProfitReconcilerConfig cfg;
    cfg.enabled = true;
    engine::TakeProfitReconciler reconciler(cfg, orders, tracker, symbolInfo, 5.0, std::chrono::hours(24));

    runAwaitable(ioc, reconciler.reconcileOnce(snapshotWithPositions({shortPosition()})));

    EXPECT_EQ(orders.limitCalls, 1);
    ASSERT_TRUE(orders.lastLimitDraft.has_value());
    EXPECT_EQ(orders.lastLimitDraft->side, OrderSide::Buy);
    EXPECT_EQ(std::string(orders.lastLimitDraft->price.value()), "99.6");
}

TEST(TakeProfitReconcilerTest, SkipsPlacementWhenTrackerEntryIsOpeningInFlight) {
    boost::asio::io_context ioc;
    OrdersStub orders;
    engine::PositionTracker tracker;
    ASSERT_TRUE(tracker.reserve("BTCUSDT"));

    engine::TakeProfitReconcilerConfig cfg;
    cfg.enabled = true;
    engine::TakeProfitReconciler reconciler(cfg, orders, tracker, symbolInfo, 5.0, std::chrono::hours(24));

    runAwaitable(ioc, reconciler.reconcileOnce(snapshotWithPositions({longPosition()})));

    EXPECT_EQ(orders.limitCalls, 0);
}

TEST(TakeProfitReconcilerTest, NoopWhenReconcilerDisabled) {
    boost::asio::io_context ioc;
    OrdersStub orders;
    engine::PositionTracker tracker;

    engine::TakeProfitReconcilerConfig cfg;
    cfg.enabled = false;
    engine::TakeProfitReconciler reconciler(cfg, orders, tracker, symbolInfo, 5.0, std::chrono::hours(24));

    runAwaitable(ioc, reconciler.reconcileOnce(snapshotWithPositions({longPosition()})));

    EXPECT_EQ(orders.openNormalOrdersCalls, 0);
    EXPECT_EQ(orders.limitCalls, 0);
}

TEST(TakeProfitReconcilerTest, NoopWhenTakeProfitPercentNonPositive) {
    boost::asio::io_context ioc;
    OrdersStub orders;
    engine::PositionTracker tracker;

    engine::TakeProfitReconcilerConfig cfg;
    cfg.enabled = true;
    engine::TakeProfitReconciler reconciler(cfg, orders, tracker, symbolInfo, 0.0, std::chrono::hours(24));

    runAwaitable(ioc, reconciler.reconcileOnce(snapshotWithPositions({longPosition()})));

    EXPECT_EQ(orders.openNormalOrdersCalls, 0);
    EXPECT_EQ(orders.limitCalls, 0);
}

TEST(TakeProfitReconcilerTest, SkipsPositionWithInvalidLeverage) {
    boost::asio::io_context ioc;
    OrdersStub orders;
    engine::PositionTracker tracker;
    auto invalid = longPosition();
    invalid.leverage = 0;

    engine::TakeProfitReconcilerConfig cfg;
    cfg.enabled = true;
    engine::TakeProfitReconciler reconciler(cfg, orders, tracker, symbolInfo, 5.0, std::chrono::hours(24));

    runAwaitable(ioc, reconciler.reconcileOnce(snapshotWithPositions({invalid})));

    EXPECT_EQ(orders.limitCalls, 0);
}

TEST(TakeProfitReconcilerTest, PlacementRejectedLeavesTrackerTakeProfitUnset) {
    boost::asio::io_context ioc;
    OrdersStub orders;
    NormalPlacementResult rejected;
    rejected.state = PlacementState::Rejected;
    rejected.binanceCode = -2010;
    rejected.binanceMessage = "rejected";
    orders.limitResult = OrdersResult<NormalPlacementResult>(rejected);

    engine::PositionTracker tracker;
    engine::TakeProfitReconcilerConfig cfg;
    cfg.enabled = true;
    engine::TakeProfitReconciler reconciler(cfg, orders, tracker, symbolInfo, 5.0, std::chrono::hours(24));

    runAwaitable(ioc, reconciler.reconcileOnce(snapshotWithPositions({longPosition()})));

    EXPECT_EQ(orders.limitCalls, 1);
    const auto tracked = tracker.bySymbol("BTCUSDT");
    ASSERT_TRUE(tracked.has_value());
    EXPECT_EQ(tracked->tpOrderId, 0);
    EXPECT_TRUE(tracked->tpClientOrderId.empty());
}

TEST(TakeProfitReconcilerTest, OpenOrdersFailureAbortsWithoutPlacement) {
    boost::asio::io_context ioc;
    OrdersStub orders;
    orders.openOrdersResult = std::unexpected(BinanceError::fromParse("open-orders failed"));
    engine::PositionTracker tracker;

    engine::TakeProfitReconcilerConfig cfg;
    cfg.enabled = true;
    engine::TakeProfitReconciler reconciler(cfg, orders, tracker, symbolInfo, 5.0, std::chrono::hours(24));

    runAwaitable(ioc, reconciler.reconcileOnce(snapshotWithPositions({longPosition()})));

    EXPECT_EQ(orders.openNormalOrdersCalls, 1);
    EXPECT_EQ(orders.limitCalls, 0);
}

TEST(TakeProfitReconcilerTest, RespectsMaxOrdersPerCycleBudget) {
    boost::asio::io_context ioc;
    OrdersStub orders;
    engine::PositionTracker tracker;
    Position a = longPosition();
    Position b = longPosition();
    b.symbol = "ETHUSDT";
    Position c = longPosition();
    c.symbol = "BNBUSDT";

    engine::TakeProfitReconcilerConfig cfg;
    cfg.enabled = true;
    cfg.maxOrdersPerCycle = 2;
    engine::TakeProfitReconciler reconciler(cfg, orders, tracker, symbolInfo, 5.0, std::chrono::hours(24));

    runAwaitable(ioc, reconciler.reconcileOnce(snapshotWithPositions({a, b, c})));

    EXPECT_EQ(orders.limitCalls, 2);
}

TEST(TakeProfitReconcilerTest, AdoptsCandidateWithLargestRemainingQuantity) {
    boost::asio::io_context ioc;
    OrdersStub orders;
    NormalOrderSnapshot smaller;
    smaller.symbol = "BTCUSDT";
    smaller.orderId = 6001;
    smaller.clientOrderId = "gtp_BTCUSDT_aa_00";
    smaller.side = OrderSide::Sell;
    smaller.type = OrderType::Limit;
    smaller.status = "NEW";
    smaller.origQty = "0.01";
    smaller.executedQty = "0.005";
    smaller.reduceOnly = true;

    NormalOrderSnapshot larger = smaller;
    larger.orderId = 6002;
    larger.clientOrderId = "gtp_BTCUSDT_bb_00";
    larger.origQty = "0.02";
    larger.executedQty = "0.001";

    orders.openOrdersResult = std::vector<NormalOrderSnapshot>{smaller, larger};

    engine::PositionTracker tracker;
    engine::TrackedPosition tracked;
    tracked.symbol = "BTCUSDT";
    tracked.direction = strategy::Signal::Direction::Long;
    tracked.entryPrice = 100.0;
    tracked.quantity = 0.01;
    tracked.openedAt = std::chrono::system_clock::now();
    tracked.maxHoldDuration = std::chrono::hours(24);
    ASSERT_TRUE(tracker.add(tracked));

    engine::TakeProfitReconcilerConfig cfg;
    cfg.enabled = true;
    engine::TakeProfitReconciler reconciler(cfg, orders, tracker, symbolInfo, 5.0, std::chrono::hours(24));

    runAwaitable(ioc, reconciler.reconcileOnce(snapshotWithPositions({longPosition()})));

    EXPECT_EQ(orders.limitCalls, 0);
    const auto updated = tracker.bySymbol("BTCUSDT");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->tpOrderId, 6002);
    EXPECT_EQ(updated->tpClientOrderId, "gtp_BTCUSDT_bb_00");
}
