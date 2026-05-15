#include <gtest/gtest.h>

#include "engine/signal_engine.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <optional>

namespace {

template <typename T>
std::enable_if_t<!std::is_void_v<T>, T> runAwaitable(boost::asio::io_context& ioc, boost::asio::awaitable<T> task) {
    auto fut = boost::asio::co_spawn(ioc, std::move(task), boost::asio::use_future);
    ioc.run();
    return fut.get();
}

void runAwaitable(boost::asio::io_context& ioc, boost::asio::awaitable<void> task) {
    auto fut = boost::asio::co_spawn(ioc, std::move(task), boost::asio::use_future);
    ioc.run();
    fut.get();
}

class MockScannerPort final : public engine::IScannerPort {
public:
    explicit MockScannerPort(boost::asio::io_context& ioc) : m_ioc(ioc), m_cache(200) {}

    const scanner::KlineCache& cache() const override { return m_cache; }
    std::vector<std::string> symbols() const override { return m_symbols; }
    std::optional<ExchangeSymbol> symbolInfo(std::string_view symbol) const override {
        auto it = m_meta.find(std::string(symbol));
        if (it == m_meta.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    boost::asio::io_context& ioContext() override { return m_ioc; }

    void setSymbols(std::vector<std::string> symbols) { m_symbols = std::move(symbols); }
    void setStep(std::string symbol, double step) {
        ExchangeSymbol meta;
        meta.symbol = std::move(symbol);
        meta.stepSize = step;
        m_meta[meta.symbol] = meta;
    }
    void push(std::string_view symbol, std::string_view interval, const Kline& kline) {
        m_cache.update(symbol, interval, kline);
    }

private:
    boost::asio::io_context& m_ioc;
    scanner::KlineCache m_cache;
    std::vector<std::string> m_symbols;
    std::unordered_map<std::string, ExchangeSymbol> m_meta;
};

class MockAccountPort final : public engine::IAccountPort {
public:
    account::AccountServiceResult<account::AccountSnapshot> nextSnapshot = account::AccountSnapshot{};

    boost::asio::awaitable<account::AccountServiceResult<account::AccountSnapshot>> snapshot(
        account::AccountSnapshotRequest) override {
        co_return nextSnapshot;
    }
};

class MockOrdersPort final : public engine::IOrdersPort {
public:
    int marketCalls{0};
    int limitCalls{0};
    int protectionCalls{0};
    int closeCalls{0};
    int cancelNormalByOrderIdCalls{0};
    int cancelAlgoByAlgoIdCalls{0};
    std::optional<LimitOrderDraft> lastLimitDraft;
    std::optional<ProtectionOrderDraft> lastProtectionDraft;
    std::optional<CloseByMarketDraft> lastCloseDraft;

    OrdersResult<NormalPlacementResult> marketResult = [] {
        NormalPlacementResult result;
        result.state = PlacementState::Accepted;
        result.orderId = 1;
        result.avgPrice = "105";
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

    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> market(MarketOrderDraft) override {
        ++marketCalls;
        co_return marketResult;
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> limit(LimitOrderDraft draft) override {
        ++limitCalls;
        lastLimitDraft = std::move(draft);
        co_return limitResult;
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> protection(ProtectionOrderDraft draft) override {
        ++protectionCalls;
        lastProtectionDraft = std::move(draft);
        co_return protectionResult;
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> closeByMarket(CloseByMarketDraft draft) override {
        ++closeCalls;
        lastCloseDraft = std::move(draft);
        NormalPlacementResult result;
        result.state = PlacementState::Accepted;
        co_return result;
    }
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelNormalByOrderId(Symbol, int64_t) override {
        ++cancelNormalByOrderIdCalls;
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

class MockStrategy final : public strategy::IStrategy {
public:
    explicit MockStrategy(strategy::StrategyConfig cfg) : m_cfg(std::move(cfg)) {}
    const strategy::StrategyConfig& config() const override { return m_cfg; }
    strategy::Signal evaluate(std::string_view, std::string_view, const std::vector<Kline>&) const override {
        return nextSignal;
    }

    mutable strategy::Signal nextSignal;

private:
    strategy::StrategyConfig m_cfg;
};

engine::WorkItem singleWork(const strategy::IStrategy* strategy) {
    return engine::WorkItem{
        .symbol = "BTCUSDT",
        .interval = "15m",
        .strategy = strategy,
    };
}

} // namespace

TEST(SignalEngineTest, CoversNoSignalLowConfidenceAtrZeroExistingAndSuccess) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 30; ++i) {
        Kline k;
        k.openTime = 1000 + i;
        k.high = 110.0;
        k.low = 90.0;
        k.close = 100.0;
        scanner.push("BTCUSDT", "15m", k);
    }

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"15m"};
    cfg.minConfidence = 0.5;
    cfg.atrPeriod = 14;
    auto strategy = std::make_unique<MockStrategy>(cfg);
    auto* strategyPtr = strategy.get();
    registry.add(std::move(strategy));

    engine::SignalEngine engine(scanner, registry, account, orders, {});

    strategyPtr->nextSignal = {};
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr)));
    EXPECT_EQ(orders.marketCalls, 0);
    ioc.restart();

    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 0.2,
        .atr = 5.0,
    };
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr)));
    EXPECT_EQ(orders.marketCalls, 0);
    ioc.restart();

    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 0.9,
        .atr = 0.0,
    };
    // Use too few candles so fallback ATR is zero.
    MockScannerPort sparseScanner(ioc);
    sparseScanner.setSymbols({"BTCUSDT"});
    sparseScanner.setStep("BTCUSDT", 0.001);
    Kline one;
    one.openTime = 1;
    one.close = 100.0;
    sparseScanner.push("BTCUSDT", "15m", one);
    engine::SignalEngine sparseEngine(sparseScanner, registry, account, orders, {});
    runAwaitable(ioc, sparseEngine.processItem(singleWork(strategyPtr)));
    EXPECT_EQ(orders.marketCalls, 0);
    ioc.restart();

    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 0.9,
        .atr = 5.0,
    };
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr)));
    EXPECT_EQ(orders.marketCalls, 1);
    ASSERT_TRUE(orders.lastLimitDraft.has_value());
    ASSERT_TRUE(orders.lastProtectionDraft.has_value());
    EXPECT_DOUBLE_EQ(orders.lastLimitDraft->price.toDouble(), 120.0);
    EXPECT_DOUBLE_EQ(orders.lastProtectionDraft->triggerPrice.toDouble(), 97.5);

    ioc.restart();
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr)));
    EXPECT_EQ(orders.marketCalls, 1);
}

TEST(SignalEngineTest, FilledTpSlClientOrderRemovesTrackedPosition) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 2000 + i;
        k.high = 110.0;
        k.low = 90.0;
        k.close = 100.0;
        scanner.push("BTCUSDT", "15m", k);
    }

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"15m"};
    cfg.minConfidence = 0.1;
    auto strategy = std::make_unique<MockStrategy>(cfg);
    auto* strategyPtr = strategy.get();
    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 1.0,
        .atr = 5.0,
    };
    registry.add(std::move(strategy));

    engine::SignalEngine engine(scanner, registry, account, orders, {});
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr)));
    ASSERT_TRUE(engine.tracker().has("BTCUSDT"));
    const auto tracked = engine.tracker().bySymbol("BTCUSDT");
    ASSERT_TRUE(tracked.has_value());

    OrderUpdateEvent fill;
    fill.symbol = "BTCUSDT";
    fill.orderStatus = "FILLED";
    fill.clientOrderId = tracked->tpClientOrderId;
    engine.onUserDataEvent(fill);

    EXPECT_FALSE(engine.tracker().has("BTCUSDT"));
}

TEST(SignalEngineTest, TimeExitCancelsExitsClosesPositionAndRemovesTracker) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    MockAccountPort account;
    MockOrdersPort orders;
    strategy::StrategyRegistry registry;
    engine::SignalEngine engine(scanner, registry, account, orders, {});

    engine::TrackedPosition pos;
    pos.symbol = "BTCUSDT";
    pos.direction = strategy::Signal::Direction::Long;
    pos.openedAt = std::chrono::system_clock::now() - std::chrono::hours(2);
    pos.maxHoldDuration = std::chrono::hours(1);
    pos.quantity = 0.01;
    pos.tpOrderId = 10;
    pos.slOrderId = 20;
    engine.tracker().add(pos);

    runAwaitable(ioc, engine.processExpiredPositions(std::chrono::system_clock::now()));

    EXPECT_EQ(orders.cancelNormalByOrderIdCalls, 1);
    EXPECT_EQ(orders.cancelAlgoByAlgoIdCalls, 1);
    EXPECT_EQ(orders.closeCalls, 1);
    ASSERT_TRUE(orders.lastCloseDraft.has_value());
    EXPECT_EQ(orders.lastCloseDraft->side, OrderSide::Sell);
    EXPECT_FALSE(engine.tracker().has("BTCUSDT"));
}
