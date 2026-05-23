#include <gtest/gtest.h>

#include "engine/signal_engine.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <optional>
#include <string>
#include <unordered_map>
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

class ScannerStub final : public engine::IScannerPort {
public:
    explicit ScannerStub(boost::asio::io_context& ioc) : m_ioc(ioc), m_cache(200) {}

    const scanner::KlineCache& cache() const override { return m_cache; }
    std::vector<std::string> symbols() const override { return {}; }
    std::optional<ExchangeSymbol> symbolInfo(std::string_view symbol) const override {
        auto it = m_symbols.find(std::string(symbol));
        if (it == m_symbols.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    boost::asio::io_context& ioContext() override { return m_ioc; }

    void setRules(std::string symbol, double stepSize, double tickSize, double minNotional = 1.0) {
        ExchangeSymbol meta;
        meta.symbol = std::move(symbol);
        meta.stepSize = stepSize;
        meta.tickSize = tickSize;
        meta.minNotional = minNotional;
        m_symbols[meta.symbol] = meta;
    }

private:
    boost::asio::io_context& m_ioc;
    scanner::KlineCache m_cache;
    std::unordered_map<std::string, ExchangeSymbol> m_symbols;
};

class AccountStub final : public engine::IAccountPort {
public:
    account::AccountServiceResult<account::AccountSnapshot> nextSnapshot = account::AccountSnapshot{};
    int calls{0};

    boost::asio::awaitable<account::AccountServiceResult<account::AccountSnapshot>> snapshot(
        account::AccountSnapshotRequest) override {
        ++calls;
        co_return nextSnapshot;
    }
};

class OrdersStub final : public engine::IOrdersPort {
public:
    int marketCalls{0};
    int limitCalls{0};
    int protectionCalls{0};
    int closeCalls{0};
    int setLeverageCalls{0};
    int amendLimitOrderCalls{0};
    int cancelNormalByOrderIdCalls{0};
    int cancelNormalByClientOrderIdCalls{0};
    int cancelAlgoByAlgoIdCalls{0};
    int cancelAlgoByClientAlgoIdCalls{0};

    OrdersResult<NormalPlacementResult> marketResult = [] {
        NormalPlacementResult result;
        result.state = PlacementState::Accepted;
        result.orderId = 1;
        result.avgPrice = "100";
        return OrdersResult<NormalPlacementResult>(result);
    }();
    OrdersResult<NormalPlacementResult> limitResult = [] {
        NormalPlacementResult result;
        result.state = PlacementState::Accepted;
        result.orderId = 2;
        return OrdersResult<NormalPlacementResult>(result);
    }();
    OrdersResult<NormalPlacementResult> protectionResult = [] {
        NormalPlacementResult result;
        result.state = PlacementState::Accepted;
        result.orderId = 3;
        return OrdersResult<NormalPlacementResult>(result);
    }();
    OrdersResult<NormalOrderSnapshot> amendLimitResult = NormalOrderSnapshot{};
    OrdersResult<NormalPlacementResult> closeResult = NormalPlacementResult{};
    OrdersResult<NormalCancelResult> cancelResult = NormalCancelResult{};

    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> market(MarketOrderDraft) override {
        ++marketCalls;
        co_return marketResult;
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> limit(LimitOrderDraft) override {
        ++limitCalls;
        co_return limitResult;
    }
    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> amendLimitOrder(AmendLimitOrderDraft) override {
        ++amendLimitOrderCalls;
        co_return amendLimitResult;
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> protection(ProtectionOrderDraft) override {
        ++protectionCalls;
        co_return protectionResult;
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> closeByMarket(CloseByMarketDraft) override {
        ++closeCalls;
        co_return closeResult;
    }
    boost::asio::awaitable<OrdersResult<LeverageResult>> setLeverage(Symbol symbol, int leverage) override {
        ++setLeverageCalls;
        co_return LeverageResult{
            .symbol = std::move(symbol),
            .leverage = leverage,
            .maxNotionalValue = 0.0,
        };
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
        ++cancelAlgoByAlgoIdCalls;
        co_return cancelResult;
    }
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelAlgoByClientAlgoId(Symbol, ClientAlgoId) override {
        ++cancelAlgoByClientAlgoIdCalls;
        co_return cancelResult;
    }
};

class OrderCapStub final : public engine::IOrderCapPort {
public:
    engine::OrderCapResult nextResult{
        .decision = engine::OrderCapDecision::Allow,
        .reason = "ok",
    };
    int calls{0};

    engine::OrderCapResult check(
        double,
        const account::AccountSnapshot&,
        const engine::PositionTracker&) const override {
        ++const_cast<OrderCapStub*>(this)->calls;
        return nextResult;
    }

    engine::OrderCapFailureMode failureMode() const override {
        return engine::OrderCapFailureMode::Closed;
    }
};

class ExposureStub final : public engine::IExposurePort {
public:
    engine::ExposureCheckResult nextResult{
        .decision = engine::ExposureDecision::Allow,
        .scaleFactor = 1.0,
        .reason = "ok",
    };
    int calls{0};

    engine::ExposureCheckResult check(
        std::string_view,
        strategy::Signal::Direction,
        double,
        const engine::PositionTracker&,
        const account::AccountSnapshot&,
        double) const override {
        ++const_cast<ExposureStub*>(this)->calls;
        return nextResult;
    }

    engine::ExposureMetrics currentMetrics(
        const engine::PositionTracker&,
        const account::AccountSnapshot&,
        double) const override {
        return {};
    }

    engine::ExposureFailureMode failureMode() const override {
        return engine::ExposureFailureMode::Closed;
    }

    double minNotionalAfterScale() const override {
        return 5.0;
    }
};

strategy::StrategyConfig baseConfig() {
    strategy::StrategyConfig cfg;
    cfg.name = "preflight-test";
    cfg.type = "golden_crossover";
    cfg.riskPct = 0.01;
    cfg.slMultiplier = 1.5;
    cfg.tpMultiplier = 0.0;
    cfg.takeProfitPercent = 0.0;
    cfg.leverage = 5;
    cfg.minNotional = 1.0;
    return cfg;
}

engine::SignalEngine::OpenPositionRequest openRequest(strategy::StrategyConfig cfg = baseConfig()) {
    return engine::SignalEngine::OpenPositionRequest{
        .symbol = "BTCUSDT",
        .signalInterval = "15m",
        .direction = strategy::Signal::Direction::Long,
        .atr = 5.0,
        .currentPrice = 100.0,
        .cfg = std::move(cfg),
        .signalReason = "preflight test",
    };
}

}  // namespace

TEST(SignalEnginePreflightTest, PreflightBuildsOrderPlanWithoutCallingOrderApi) {
    boost::asio::io_context ioc;
    ScannerStub scanner(ioc);
    scanner.setRules("BTCUSDT", 0.001, 0.01);

    AccountStub account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    OrdersStub orders;
    OrderCapStub orderCap;
    ExposureStub exposure;
    strategy::StrategyRegistry registry;
    engine::SignalEngine engine(
        scanner,
        registry,
        account,
        orders,
        orderCap,
        exposure,
        engine::SignalEngine::Config{.placeStopLoss = false});

    auto result = runAwaitable(ioc, engine.preflightOpenPosition(openRequest()));

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_TRUE((*result)->quantity.has_value());
    EXPECT_GT((*result)->size.quantity, 0.0);
    EXPECT_EQ(account.calls, 1);
    EXPECT_EQ(orderCap.calls, 1);
    EXPECT_EQ(exposure.calls, 1);
    EXPECT_EQ(orders.setLeverageCalls, 0);
    EXPECT_EQ(orders.marketCalls, 0);
    EXPECT_EQ(orders.limitCalls, 0);
    EXPECT_EQ(orders.protectionCalls, 0);
}

TEST(SignalEnginePreflightTest, OrderCapBlockStopsBeforeExposureAndOrders) {
    boost::asio::io_context ioc;
    ScannerStub scanner(ioc);
    scanner.setRules("BTCUSDT", 0.001, 0.01);

    AccountStub account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    OrdersStub orders;
    OrderCapStub orderCap;
    orderCap.nextResult = {
        .decision = engine::OrderCapDecision::Block,
        .reason = "cap reached",
    };
    ExposureStub exposure;
    strategy::StrategyRegistry registry;
    engine::SignalEngine engine(
        scanner,
        registry,
        account,
        orders,
        orderCap,
        exposure,
        engine::SignalEngine::Config{.placeStopLoss = false});

    auto result = runAwaitable(ioc, engine.preflightOpenPosition(openRequest()));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
    EXPECT_EQ(orderCap.calls, 1);
    EXPECT_EQ(exposure.calls, 0);
    EXPECT_EQ(orders.setLeverageCalls, 0);
    EXPECT_EQ(orders.marketCalls, 0);
}

TEST(SignalEnginePreflightTest, OpenFromPreflightPlacesOrders) {
    boost::asio::io_context ioc;
    ScannerStub scanner(ioc);
    scanner.setRules("BTCUSDT", 0.001, 0.01);

    AccountStub account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    OrdersStub orders;
    OrderCapStub orderCap;
    ExposureStub exposure;
    strategy::StrategyRegistry registry;
    engine::SignalEngine engine(
        scanner,
        registry,
        account,
        orders,
        orderCap,
        exposure,
        engine::SignalEngine::Config{.placeStopLoss = false});

    auto preflight = runAwaitable(ioc, engine.preflightOpenPosition(openRequest()));
    ASSERT_TRUE(preflight.has_value());
    ASSERT_TRUE(preflight->has_value());

    auto opened = runAwaitable(ioc, engine.openPositionFromPreflight(std::move(**preflight)));

    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ(orders.setLeverageCalls, 1);
    EXPECT_EQ(orders.marketCalls, 1);
    EXPECT_TRUE(engine.tracker().has("BTCUSDT"));
}
