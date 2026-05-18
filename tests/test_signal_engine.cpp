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
#include <stdexcept>

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
    void setRules(std::string symbol, double step, double tick) {
        ExchangeSymbol meta;
        meta.symbol = std::move(symbol);
        meta.stepSize = step;
        meta.tickSize = tick;
        m_meta[meta.symbol] = meta;
    }
    void setRules(std::string symbol, double step, double tick, double minNotional) {
        ExchangeSymbol meta;
        meta.symbol = std::move(symbol);
        meta.stepSize = step;
        meta.tickSize = tick;
        meta.minNotional = minNotional;
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
    int calls{0};
    account::AccountSnapshotRequest lastRequest{};

    boost::asio::awaitable<account::AccountServiceResult<account::AccountSnapshot>> snapshot(
        account::AccountSnapshotRequest request) override {
        ++calls;
        lastRequest = request;
        co_return nextSnapshot;
    }
};

class MockOrdersPort final : public engine::IOrdersPort {
public:
    int marketCalls{0};
    int limitCalls{0};
    int protectionCalls{0};
    int closeCalls{0};
    int setLeverageCalls{0};
    int cancelNormalByOrderIdCalls{0};
    int cancelAlgoByAlgoIdCalls{0};
    int cancelAlgoByClientAlgoIdCalls{0};
    std::string lastSetLeverageSymbol;
    int lastRequestedLeverage{0};
    std::optional<MarketOrderDraft> lastMarketDraft;
    std::optional<LimitOrderDraft> lastLimitDraft;
    std::optional<ProtectionOrderDraft> lastProtectionDraft;
    std::optional<CloseByMarketDraft> lastCloseDraft;
    std::optional<int> leverageResponseOverride;
    std::optional<BinanceError> setLeverageError;

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
    boost::asio::awaitable<OrdersResult<LeverageResult>> setLeverage(Symbol symbol, int leverage) override {
        ++setLeverageCalls;
        lastSetLeverageSymbol = std::move(symbol);
        lastRequestedLeverage = leverage;
        if (setLeverageError.has_value()) {
            co_return std::unexpected(*setLeverageError);
        }
        co_return LeverageResult{
            .symbol = lastSetLeverageSymbol,
            .leverage = leverageResponseOverride.value_or(leverage),
            .maxNotionalValue = 0.0,
        };
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
        ++cancelAlgoByClientAlgoIdCalls;
        co_return NormalCancelResult{};
    }
};

class MockExposurePort final : public engine::IExposurePort {
public:
    engine::ExposureCheckResult nextResult{
        .decision = engine::ExposureDecision::Allow,
        .scaleFactor = 1.0,
    };
    engine::ExposureFailureMode mode{engine::ExposureFailureMode::Closed};
    double minNotional{5.0};
    bool throwOnCheck{false};
    int checkCalls{0};
    double lastProposedNotional{0.0};

    engine::ExposureCheckResult check(
        std::string_view,
        strategy::Signal::Direction,
        double proposedNotional,
        const engine::PositionTracker&,
        const account::AccountSnapshot&,
        double) const override {
        ++const_cast<MockExposurePort*>(this)->checkCalls;
        const_cast<MockExposurePort*>(this)->lastProposedNotional = proposedNotional;
        if (throwOnCheck) {
            throw std::runtime_error("mock exposure failure");
        }
        return nextResult;
    }

    engine::ExposureMetrics currentMetrics(
        const engine::PositionTracker&,
        const account::AccountSnapshot&,
        double) const override {
        return {};
    }

    engine::ExposureFailureMode failureMode() const override {
        return mode;
    }

    double minNotionalAfterScale() const override {
        return minNotional;
    }
};

class MockOrderCapPort final : public engine::IOrderCapPort {
public:
    engine::OrderCapResult nextResult{
        .decision = engine::OrderCapDecision::Allow,
        .reason = "ok",
    };
    engine::OrderCapFailureMode mode{engine::OrderCapFailureMode::Closed};
    bool throwOnCheck{false};
    int checkCalls{0};

    engine::OrderCapResult check(
        double,
        const account::AccountSnapshot&,
        const engine::PositionTracker&) const override {
        ++const_cast<MockOrderCapPort*>(this)->checkCalls;
        if (throwOnCheck) {
            throw std::runtime_error("mock order cap failure");
        }
        return nextResult;
    }

    engine::OrderCapFailureMode failureMode() const override {
        return mode;
    }
};

class MockGeminiFilterPort final : public engine::IGeminiFilterPort {
public:
    engine::GeminiFilterResult nextResult{
        .decision = engine::GeminiDecision::Allow,
        .confidence = 1.0,
        .sentimentScore = 1.0,
        .visionScore = 1.0,
        .reason = "ok",
    };
    int evaluateCalls{0};
    std::string lastInterval;

    engine::GeminiFilterResult evaluate(
        std::string_view,
        strategy::Signal::Direction,
        std::string_view signalInterval,
        const scanner::KlineCache&) const override {
        ++const_cast<MockGeminiFilterPort*>(this)->evaluateCalls;
        const_cast<MockGeminiFilterPort*>(this)->lastInterval = std::string(signalInterval);
        return nextResult;
    }
};

class MockRiskPort final : public engine::IRiskPort {
public:
    bool canOpen{true};
    int canOpenCalls{0};
    int onPositionClosedCalls{0};
    int onScanCycleCalls{0};
    int maybeRecomputeCalls{0};
    int64_t lastOnScanTs{0};
    int64_t lastOnClosedTs{0};
    int64_t lastRecomputeTs{0};
    engine::RiskStatus status{engine::RiskStatus::OK};

    bool canOpenPosition() const override {
        ++const_cast<MockRiskPort*>(this)->canOpenCalls;
        return canOpen;
    }

    void onPositionClosed(const account::AccountSnapshot&, int64_t timestampMs) override {
        ++onPositionClosedCalls;
        lastOnClosedTs = timestampMs;
    }

    void onScanCycle(const account::AccountSnapshot&, int64_t timestampMs) override {
        ++onScanCycleCalls;
        lastOnScanTs = timestampMs;
    }

    boost::asio::awaitable<void> maybeRecompute(int64_t nowMs) override {
        ++maybeRecomputeCalls;
        lastRecomputeTs = nowMs;
        co_return;
    }

    engine::RiskStatus currentStatus() const override {
        return status;
    }
};

class MockStrategy final : public strategy::IStrategy {
public:
    explicit MockStrategy(strategy::StrategyConfig cfg) : m_cfg(std::move(cfg)) {}
    const strategy::StrategyConfig& config() const override { return m_cfg; }
    strategy::Signal evaluate(std::string_view, std::string_view interval, const std::vector<Kline>&) const override {
        ++evaluateCalls;
        lastInterval = std::string(interval);
        return nextSignal;
    }

    mutable int evaluateCalls{0};
    mutable std::string lastInterval;
    mutable strategy::Signal nextSignal;

private:
    strategy::StrategyConfig m_cfg;
};

engine::WorkItem singleWork(const strategy::IStrategy* strategy, std::string interval = "15m") {
    return engine::WorkItem{
        .symbol = "BTCUSDT",
        .interval = std::move(interval),
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

    MockExposurePort exposure;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, {});

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
    engine::SignalEngine sparseEngine(sparseScanner, registry, account, orders, exposure, {});
    runAwaitable(ioc, sparseEngine.processItem(singleWork(strategyPtr)));
    EXPECT_EQ(orders.marketCalls, 0);
    ioc.restart();

    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 0.9,
        .atr = 5.0,
        .reason = "15m Donchian breakout long: close=100 > high20=99",
    };
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr)));
    EXPECT_EQ(orders.marketCalls, 1);
    EXPECT_TRUE(account.lastRequest.includePositions);
    ASSERT_TRUE(orders.lastMarketDraft.has_value());
    ASSERT_TRUE(orders.lastLimitDraft.has_value());
    ASSERT_TRUE(orders.lastProtectionDraft.has_value());
    ASSERT_TRUE(orders.lastMarketDraft->metadata.has_value());
    ASSERT_TRUE(orders.lastLimitDraft->metadata.has_value());
    ASSERT_TRUE(orders.lastProtectionDraft->metadata.has_value());
    EXPECT_EQ(orders.lastMarketDraft->metadata->strategyTag.value_or(""), "mock");
    EXPECT_EQ(orders.lastLimitDraft->metadata->strategyTag.value_or(""), "mock");
    EXPECT_EQ(orders.lastProtectionDraft->metadata->strategyTag.value_or(""), "mock");
    EXPECT_EQ(orders.lastMarketDraft->metadata->timeframe.value_or(""), "15m");
    EXPECT_EQ(orders.lastLimitDraft->metadata->timeframe.value_or(""), "15m");
    EXPECT_EQ(orders.lastProtectionDraft->metadata->timeframe.value_or(""), "15m");
    EXPECT_EQ(
        orders.lastMarketDraft->metadata->comment.value_or(""),
        "tf=15m reason=15m Donchian breakout long: close=100 > high20=99");
    ASSERT_EQ(orders.setLeverageCalls, 1);
    EXPECT_EQ(orders.lastSetLeverageSymbol, "BTCUSDT");
    EXPECT_GE(orders.lastRequestedLeverage, 2);
    EXPECT_LE(orders.lastRequestedLeverage, 20);
    const double expectedTp = 105.0 + (105.0 * cfg.takeProfitPercent / (100.0 * orders.lastRequestedLeverage));
    EXPECT_NEAR(orders.lastLimitDraft->price.toDouble(), expectedTp, 1e-9);
    EXPECT_DOUBLE_EQ(orders.lastProtectionDraft->triggerPrice.toDouble(), 97.5);
    const auto tracked = engine.tracker().bySymbol("BTCUSDT");
    ASSERT_TRUE(tracked.has_value());
    EXPECT_EQ(tracked->strategyName, "mock");
    EXPECT_EQ(tracked->signalInterval, "15m");
    EXPECT_EQ(tracked->signalReason, "15m Donchian breakout long: close=100 > high20=99");
    EXPECT_EQ(tracked->trailingInterval, "15m");

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

    MockExposurePort exposure;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, {});
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

TEST(SignalEngineTest, CanDisableStopLossPlacement) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 2500 + i;
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

    MockExposurePort exposure;
    engine::SignalEngine engine(
        scanner,
        registry,
        account,
        orders,
        exposure,
        engine::SignalEngine::Config{.placeStopLoss = false});
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr)));

    EXPECT_EQ(orders.marketCalls, 1);
    EXPECT_EQ(orders.limitCalls, 1);
    EXPECT_EQ(orders.protectionCalls, 0);
    ASSERT_TRUE(engine.tracker().has("BTCUSDT"));
    const auto tracked = engine.tracker().bySymbol("BTCUSDT");
    ASSERT_TRUE(tracked.has_value());
    EXPECT_TRUE(tracked->slClientOrderId.empty());
    EXPECT_EQ(tracked->slOrderId, 0);
}

TEST(SignalEngineTest, TakeProfitPriceIsRoundedToSymbolTickSize) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setRules("BTCUSDT", 0.001, 0.01);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 2600 + i;
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
    orders.marketResult->avgPrice = "105.003";

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"15m"};
    cfg.minConfidence = 0.1;
    cfg.tpMultiplier = 1.0;
    cfg.takeProfitPercent = 0.0;
    auto strategy = std::make_unique<MockStrategy>(cfg);
    auto* strategyPtr = strategy.get();
    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 1.0,
        .atr = 0.123456,
    };
    registry.add(std::move(strategy));

    MockExposurePort exposure;
    engine::SignalEngine engine(
        scanner,
        registry,
        account,
        orders,
        exposure,
        engine::SignalEngine::Config{.placeStopLoss = false});
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr)));

    ASSERT_TRUE(orders.lastLimitDraft.has_value());
    EXPECT_EQ(orders.lastLimitDraft->price.value(), "105.12");
}

TEST(SignalEngineTest, TakeProfitPercentUsesBinanceRoiWithLeverageForLong) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setRules("BTCUSDT", 0.001, 0.01);

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    snapshot.positions = std::vector<Position>{
        Position{
            .symbol = "BTCUSDT",
            .leverage = 20,
        },
    };
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    orders.marketResult->avgPrice = "100";
    orders.leverageResponseOverride = 20;

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.takeProfitPercent = 20.0;

    MockExposurePort exposure;
    engine::SignalEngine engine(
        scanner,
        registry,
        account,
        orders,
        exposure,
        engine::SignalEngine::Config{.placeStopLoss = false});

    const auto result = runAwaitable(
        ioc,
        engine.openPosition(
            "BTCUSDT",
            "15m",
            strategy::Signal::Direction::Long,
            5.0,
            100.0,
            cfg,
            "roi tp test"));

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(orders.setLeverageCalls, 1);
    EXPECT_EQ(orders.lastSetLeverageSymbol, "BTCUSDT");
    EXPECT_GE(orders.lastRequestedLeverage, 2);
    EXPECT_LE(orders.lastRequestedLeverage, 20);
    ASSERT_TRUE(orders.lastLimitDraft.has_value());
    EXPECT_EQ(orders.lastLimitDraft->price.value(), "101");
}

TEST(SignalEngineTest, TakeProfitPercentUsesBinanceRoiWithLeverageForShort) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setRules("BTCUSDT", 0.001, 0.01);

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    snapshot.account.positions = {
        Position{
            .symbol = "BTCUSDT",
            .leverage = 20,
        },
    };
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    orders.marketResult->avgPrice = "100";
    orders.leverageResponseOverride = 20;

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.takeProfitPercent = 20.0;

    MockExposurePort exposure;
    engine::SignalEngine engine(
        scanner,
        registry,
        account,
        orders,
        exposure,
        engine::SignalEngine::Config{.placeStopLoss = false});

    const auto result = runAwaitable(
        ioc,
        engine.openPosition(
            "BTCUSDT",
            "15m",
            strategy::Signal::Direction::Short,
            5.0,
            100.0,
            cfg,
            "roi tp test"));

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(orders.setLeverageCalls, 1);
    EXPECT_EQ(orders.lastSetLeverageSymbol, "BTCUSDT");
    EXPECT_GE(orders.lastRequestedLeverage, 2);
    EXPECT_LE(orders.lastRequestedLeverage, 20);
    ASSERT_TRUE(orders.lastLimitDraft.has_value());
    EXPECT_EQ(orders.lastLimitDraft->price.value(), "99");
}

TEST(SignalEngineTest, OpenPositionUsesPerIntervalMaxHoldDurationWhenConfigured) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setRules("BTCUSDT", 0.001, 0.01);

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    orders.marketResult->avgPrice = "100";

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.maxHoldDuration = std::chrono::hours(24);
    cfg.maxHoldDurationByInterval = {
        {"4h", std::chrono::hours(24 * 5)},
        {"1h", std::chrono::hours(30)},
    };

    MockExposurePort exposure;
    engine::SignalEngine engine(
        scanner,
        registry,
        account,
        orders,
        exposure,
        engine::SignalEngine::Config{.placeStopLoss = false});

    const auto result = runAwaitable(
        ioc,
        engine.openPosition(
            "BTCUSDT",
            "4h",
            strategy::Signal::Direction::Long,
            5.0,
            100.0,
            cfg,
            "per-tf hold test"));

    ASSERT_TRUE(result.has_value());
    const auto tracked = engine.tracker().bySymbol("BTCUSDT");
    ASSERT_TRUE(tracked.has_value());
    EXPECT_EQ(tracked->maxHoldDuration, std::chrono::hours(24 * 5));
}

TEST(SignalEngineTest, SetLeverageFailureSkipsMarketOrder) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setRules("BTCUSDT", 0.001, 0.01);

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    orders.setLeverageError = BinanceError::fromApiResponse(-4003, "invalid leverage");

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.takeProfitPercent = 20.0;

    MockExposurePort exposure;
    engine::SignalEngine engine(
        scanner,
        registry,
        account,
        orders,
        exposure,
        engine::SignalEngine::Config{.placeStopLoss = false});

    const auto result = runAwaitable(
        ioc,
        engine.openPosition(
            "BTCUSDT",
            "15m",
            strategy::Signal::Direction::Long,
            5.0,
            100.0,
            cfg,
            "leverage failure test"));

    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(orders.setLeverageCalls, 1);
    EXPECT_GE(orders.lastRequestedLeverage, 2);
    EXPECT_LE(orders.lastRequestedLeverage, 20);
    EXPECT_EQ(orders.marketCalls, 0);
    EXPECT_FALSE(engine.tracker().has("BTCUSDT"));
}

TEST(SignalEngineTest, MarketQuantityUsesSymbolMinNotional) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setRules("BTCUSDT", 0.01, 0.01, 5.0);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 2700 + i;
        k.high = 350.0;
        k.low = 320.0;
        k.close = 333.0;
        scanner.push("BTCUSDT", "15m", k);
    }

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 100.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"15m"};
    cfg.minConfidence = 0.1;
    cfg.minNotional = 1.0;
    auto strategy = std::make_unique<MockStrategy>(cfg);
    auto* strategyPtr = strategy.get();
    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 1.0,
        .atr = 100.0,
    };
    registry.add(std::move(strategy));

    MockExposurePort exposure;
    exposure.minNotional = 0.5;
    engine::SignalEngine engine(
        scanner,
        registry,
        account,
        orders,
        exposure,
        engine::SignalEngine::Config{.minNotional = 1.0, .placeStopLoss = false});
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr)));

    ASSERT_TRUE(orders.lastMarketDraft.has_value());
    EXPECT_EQ(orders.lastMarketDraft->quantity.value(), "0.02");
    EXPECT_DOUBLE_EQ(orders.lastMarketDraft->quantity.toDouble(), 0.02);
}

TEST(SignalEngineTest, TimeExitCancelsExitsClosesPositionAndRemovesTracker) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    MockAccountPort account;
    account::AccountSnapshot liveSnapshot;
    Position livePosition;
    livePosition.symbol = "BTCUSDT";
    livePosition.positionAmt = 0.01;
    liveSnapshot.positions = std::vector<Position>{livePosition};
    account.nextSnapshot = liveSnapshot;
    MockOrdersPort orders;
    strategy::StrategyRegistry registry;
    MockExposurePort exposure;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, {});

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

TEST(SignalEngineTest, TimeExitRemovesTrackerWithoutCloseWhenLivePositionAlreadyFlat) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    MockAccountPort account;
    account::AccountSnapshot flatSnapshot;
    Position flatPosition;
    flatPosition.symbol = "BTCUSDT";
    flatPosition.positionAmt = 0.0;
    flatSnapshot.positions = std::vector<Position>{flatPosition};
    account.nextSnapshot = flatSnapshot;
    MockOrdersPort orders;
    strategy::StrategyRegistry registry;
    MockExposurePort exposure;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, {});

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

    EXPECT_EQ(orders.cancelNormalByOrderIdCalls, 0);
    EXPECT_EQ(orders.cancelAlgoByAlgoIdCalls, 0);
    EXPECT_EQ(orders.closeCalls, 0);
    EXPECT_FALSE(engine.tracker().has("BTCUSDT"));
}

TEST(SignalEngineTest, ReconcileTrackedPositionsRemovesStaleTrackerEntry) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    MockAccountPort account;
    account::AccountSnapshot flatSnapshot;
    Position flatPosition;
    flatPosition.symbol = "BTCUSDT";
    flatPosition.positionAmt = 0.0;
    flatSnapshot.positions = std::vector<Position>{flatPosition};
    account.nextSnapshot = flatSnapshot;
    MockOrdersPort orders;
    strategy::StrategyRegistry registry;
    MockExposurePort exposure;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, {});

    engine::TrackedPosition pos;
    pos.symbol = "BTCUSDT";
    pos.direction = strategy::Signal::Direction::Long;
    pos.openedAt = std::chrono::system_clock::now() - std::chrono::minutes(10);
    pos.maxHoldDuration = std::chrono::hours(24);
    pos.quantity = 0.02;
    engine.tracker().add(pos);
    ASSERT_TRUE(engine.tracker().has("BTCUSDT"));

    runAwaitable(ioc, engine.reconcileTrackedPositions());

    EXPECT_FALSE(engine.tracker().has("BTCUSDT"));
}

TEST(SignalEngineTest, EvaluatesSignalBeforeTrackedPositionSkip) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 2400 + i;
        k.high = 110.0;
        k.low = 90.0;
        k.close = 100.0;
        scanner.push("BTCUSDT", "1h", k);
    }

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"1h"};
    cfg.minConfidence = 0.1;
    auto strategy = std::make_unique<MockStrategy>(cfg);
    auto* strategyPtr = strategy.get();
    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 1.0,
        .atr = 5.0,
        .reason = "1h Donchian breakout long",
    };
    registry.add(std::move(strategy));

    MockExposurePort exposure;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, {});

    engine::TrackedPosition tracked;
    tracked.symbol = "BTCUSDT";
    tracked.direction = strategy::Signal::Direction::Long;
    tracked.quantity = 0.01;
    tracked.trailingEnabled = true;
    tracked.trailingInterval = "1h";
    engine.tracker().add(tracked);

    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr, "1h")));

    EXPECT_EQ(strategyPtr->evaluateCalls, 1);
    EXPECT_EQ(strategyPtr->lastInterval, "1h");
    EXPECT_EQ(orders.marketCalls, 0);
}

TEST(SignalEngineTest, ProcessTrailingStopsMovesLongStopAndUpdatesTracker) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    MockAccountPort account;
    MockOrdersPort orders;
    strategy::StrategyRegistry registry;
    MockExposurePort exposure;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, {});

    for (int i = 0; i < 3; ++i) {
        Kline k;
        k.openTime = 6000 + i;
        k.high = 120.0 + i;
        k.low = 100.0 + i;
        k.close = 110.0 + i;
        k.isClosed = true;
        scanner.push("BTCUSDT", "4h", k);
    }
    Kline forming;
    forming.openTime = 7000;
    forming.high = 200.0;
    forming.low = 50.0;
    forming.close = 150.0;
    forming.isClosed = false;
    scanner.push("BTCUSDT", "4h", forming);

    engine::TrackedPosition pos;
    pos.symbol = "BTCUSDT";
    pos.direction = strategy::Signal::Direction::Long;
    pos.quantity = 0.01;
    pos.slOrderId = 20;
    pos.slClientOrderId = "old_sl";
    pos.trailingEnabled = true;
    pos.trailingInterval = "4h";
    pos.trailingCandles = 3;
    pos.currentTrailLevel = 80.0;
    engine.tracker().add(pos);

    runAwaitable(ioc, engine.processTrailingStops());

    EXPECT_EQ(orders.cancelAlgoByAlgoIdCalls, 1);
    EXPECT_EQ(orders.protectionCalls, 1);
    ASSERT_TRUE(orders.lastProtectionDraft.has_value());
    EXPECT_DOUBLE_EQ(orders.lastProtectionDraft->triggerPrice.toDouble(), 100.0);
    const auto tracked = engine.tracker().bySymbol("BTCUSDT");
    ASSERT_TRUE(tracked.has_value());
    EXPECT_EQ(tracked->slOrderId, 3);
    EXPECT_DOUBLE_EQ(tracked->currentTrailLevel, 100.0);
}

TEST(SignalEngineTest, ExposureBlockSkipsOpen) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 3000 + i;
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
    MockExposurePort exposure;
    exposure.nextResult = {
        .decision = engine::ExposureDecision::Block,
        .scaleFactor = 0.0,
        .reason = "risk limit",
    };

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

    engine::SignalEngine engine(scanner, registry, account, orders, exposure, {});
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr)));
    EXPECT_EQ(exposure.checkCalls, 1);
    EXPECT_EQ(orders.marketCalls, 0);
}

TEST(SignalEngineTest, ExposureBlockSkipsRiskGate) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 8400 + i;
        k.high = 110.0;
        k.low = 90.0;
        k.close = 100.0;
        scanner.push("BTCUSDT", "1h", k);
    }

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    MockExposurePort exposure;
    exposure.nextResult = {
        .decision = engine::ExposureDecision::Block,
        .reason = "exposure blocked",
    };
    MockGeminiFilterPort gemini;
    MockRiskPort risk;

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"1h"};
    cfg.minConfidence = 0.1;
    auto strategy = std::make_unique<MockStrategy>(cfg);
    auto* strategyPtr = strategy.get();
    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 1.0,
        .atr = 5.0,
    };
    registry.add(std::move(strategy));

    engine::GeminiFilterConfig geminiCfg;
    geminiCfg.enabled = true;
    geminiCfg.mode = engine::GeminiFilterMode::Enforce;
    geminiCfg.maxEvaluationsPerScanCycle = 3;
    engine::SignalEngine signalEngine(
        scanner, registry, account, orders, exposure, gemini, geminiCfg, {}, &risk);
    runAwaitable(ioc, signalEngine.processItem(singleWork(strategyPtr, "1h")));

    EXPECT_EQ(exposure.checkCalls, 1);
    EXPECT_EQ(risk.canOpenCalls, 0);
    EXPECT_EQ(gemini.evaluateCalls, 0);
    EXPECT_EQ(orders.marketCalls, 0);
}

TEST(SignalEngineTest, RiskGateBlockSkipsGeminiAndOrderPlacement) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 8450 + i;
        k.high = 110.0;
        k.low = 90.0;
        k.close = 100.0;
        scanner.push("BTCUSDT", "1h", k);
    }

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    MockExposurePort exposure;
    MockGeminiFilterPort gemini;
    MockRiskPort risk;
    risk.canOpen = false;
    risk.status = engine::RiskStatus::HARD_BREACH;

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"1h"};
    cfg.minConfidence = 0.1;
    auto strategy = std::make_unique<MockStrategy>(cfg);
    auto* strategyPtr = strategy.get();
    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 1.0,
        .atr = 5.0,
    };
    registry.add(std::move(strategy));

    engine::GeminiFilterConfig geminiCfg;
    geminiCfg.enabled = true;
    geminiCfg.mode = engine::GeminiFilterMode::Enforce;
    geminiCfg.maxEvaluationsPerScanCycle = 3;
    engine::SignalEngine signalEngine(
        scanner, registry, account, orders, exposure, gemini, geminiCfg, {}, &risk);
    runAwaitable(ioc, signalEngine.processItem(singleWork(strategyPtr, "1h")));

    EXPECT_EQ(exposure.checkCalls, 1);
    EXPECT_EQ(risk.canOpenCalls, 1);
    EXPECT_EQ(gemini.evaluateCalls, 0);
    EXPECT_EQ(orders.marketCalls, 0);
}

TEST(SignalEngineTest, TakeProfitRejectTriggersEmergencyCloseAndSkipsTracking) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 3500 + i;
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
    NormalPlacementResult rejectedTp;
    rejectedTp.state = PlacementState::Rejected;
    rejectedTp.binanceCode = -2010;
    rejectedTp.binanceMessage = "Order would immediately trigger";
    orders.limitResult = rejectedTp;

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

    MockExposurePort exposure;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, {});
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr)));

    EXPECT_EQ(orders.marketCalls, 1);
    EXPECT_EQ(orders.limitCalls, 1);
    EXPECT_EQ(orders.closeCalls, 1);
    EXPECT_FALSE(engine.tracker().has("BTCUSDT"));
}

TEST(SignalEngineTest, PartialExitFillReducesTrackedQuantity) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    MockAccountPort account;
    MockOrdersPort orders;
    strategy::StrategyRegistry registry;
    MockExposurePort exposure;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, {});

    engine::TrackedPosition tracked;
    tracked.symbol = "BTCUSDT";
    tracked.direction = strategy::Signal::Direction::Long;
    tracked.quantity = 1.0;
    tracked.tpClientOrderId = "tp-partial";
    engine.tracker().add(tracked);

    OrderUpdateEvent partial;
    partial.symbol = "BTCUSDT";
    partial.orderStatus = "PARTIALLY_FILLED";
    partial.clientOrderId = "tp-partial";
    partial.lastFilledQty = 0.4;
    engine.onUserDataEvent(partial);

    const auto afterPartial = engine.tracker().bySymbol("BTCUSDT");
    ASSERT_TRUE(afterPartial.has_value());
    EXPECT_NEAR(afterPartial->quantity, 0.6, 1e-12);

    partial.lastFilledQty = 0.6;
    engine.onUserDataEvent(partial);
    EXPECT_FALSE(engine.tracker().has("BTCUSDT"));
}

TEST(SignalEngineTest, ExposureScaleDownReducesQuantity) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 4000 + i;
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
    MockExposurePort exposure;
    exposure.nextResult = {
        .decision = engine::ExposureDecision::ScaleDown,
        .scaleFactor = 0.5,
        .reason = "soft limit",
    };
    exposure.minNotional = 0.5;

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

    engine::SignalEngine engine(scanner, registry, account, orders, exposure, {});
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr)));
    ASSERT_TRUE(orders.lastMarketDraft.has_value());
    EXPECT_NEAR(orders.lastMarketDraft->quantity.toDouble(), 0.666, 1e-9);
}

TEST(SignalEngineTest, OrderCapBlockSkipsOpenAndExposure) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 4100 + i;
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
    MockExposurePort exposure;
    MockOrderCapPort orderCap;
    orderCap.nextResult = {
        .decision = engine::OrderCapDecision::Block,
        .reason = "cap reached",
    };

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

    engine::SignalEngine engine(scanner, registry, account, orders, orderCap, exposure, {});
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr)));
    EXPECT_EQ(orderCap.checkCalls, 1);
    EXPECT_EQ(exposure.checkCalls, 0);
    EXPECT_EQ(orders.marketCalls, 0);
}

TEST(SignalEngineTest, OrderCapExceptionHonorsFailureMode) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 4200 + i;
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
    MockExposurePort exposure;
    MockOrderCapPort orderCap;
    orderCap.throwOnCheck = true;

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

    orderCap.mode = engine::OrderCapFailureMode::Closed;
    engine::SignalEngine engineClosed(scanner, registry, account, orders, orderCap, exposure, {});
    runAwaitable(ioc, engineClosed.processItem(singleWork(strategyPtr)));
    EXPECT_EQ(orders.marketCalls, 0);
    EXPECT_EQ(exposure.checkCalls, 0);

    ioc.restart();
    orderCap.mode = engine::OrderCapFailureMode::Open;
    engine::SignalEngine engineOpen(scanner, registry, account, orders, orderCap, exposure, {});
    runAwaitable(ioc, engineOpen.processItem(singleWork(strategyPtr)));
    EXPECT_EQ(orders.marketCalls, 1);
    EXPECT_EQ(exposure.checkCalls, 1);
}

TEST(SignalEngineTest, ExposureExceptionHonorsFailureMode) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 5000 + i;
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
    MockExposurePort exposure;
    exposure.throwOnCheck = true;

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

    exposure.mode = engine::ExposureFailureMode::Closed;
    engine::SignalEngine engineClosed(scanner, registry, account, orders, exposure, {});
    runAwaitable(ioc, engineClosed.processItem(singleWork(strategyPtr)));
    EXPECT_EQ(orders.marketCalls, 0);

    ioc.restart();
    exposure.mode = engine::ExposureFailureMode::Open;
    engine::SignalEngine engineOpen(scanner, registry, account, orders, exposure, {});
    runAwaitable(ioc, engineOpen.processItem(singleWork(strategyPtr)));
    EXPECT_EQ(orders.marketCalls, 1);
}

TEST(SignalEngineTest, GeminiEnforceBlockSkipsOrderPlacement) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 8000 + i;
        k.high = 110.0;
        k.low = 90.0;
        k.close = 100.0;
        scanner.push("BTCUSDT", "1h", k);
    }

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    MockExposurePort exposure;
    MockGeminiFilterPort gemini;
    gemini.nextResult = {
        .decision = engine::GeminiDecision::Block,
        .confidence = 0.2,
        .sentimentScore = 0.3,
        .visionScore = 0.1,
        .reason = "blocked by mock",
        .errorCode = "",
        .hasError = false,
    };

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"1h"};
    cfg.minConfidence = 0.1;
    auto strategy = std::make_unique<MockStrategy>(cfg);
    auto* strategyPtr = strategy.get();
    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 1.0,
        .atr = 5.0,
    };
    registry.add(std::move(strategy));

    engine::GeminiFilterConfig geminiCfg;
    geminiCfg.enabled = true;
    geminiCfg.mode = engine::GeminiFilterMode::Enforce;
    geminiCfg.maxEvaluationsPerScanCycle = 3;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, gemini, geminiCfg, {});
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr, "1h")));

    EXPECT_EQ(gemini.evaluateCalls, 1);
    EXPECT_EQ(gemini.lastInterval, "1h");
    EXPECT_EQ(orders.marketCalls, 0);
}

TEST(SignalEngineTest, GeminiEnforceBlockWithoutErrorSkipsOrderPlacement) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 9000 + i;
        k.high = 110.0;
        k.low = 90.0;
        k.close = 100.0;
        scanner.push("BTCUSDT", "4h", k);
    }

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    MockExposurePort exposure;
    MockGeminiFilterPort gemini;
    gemini.nextResult = {
        .decision = engine::GeminiDecision::Block,
        .confidence = 0.2,
        .sentimentScore = 0.3,
        .visionScore = 0.1,
        .reason = "blocked by model confidence",
        .errorCode = "",
        .hasError = false,
    };

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"4h"};
    cfg.minConfidence = 0.1;
    auto strategy = std::make_unique<MockStrategy>(cfg);
    auto* strategyPtr = strategy.get();
    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 1.0,
        .atr = 5.0,
    };
    registry.add(std::move(strategy));

    engine::GeminiFilterConfig geminiCfg;
    geminiCfg.enabled = true;
    geminiCfg.mode = engine::GeminiFilterMode::Enforce;
    geminiCfg.maxEvaluationsPerScanCycle = 3;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, gemini, geminiCfg, {});
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr, "4h")));

    EXPECT_EQ(gemini.evaluateCalls, 1);
    EXPECT_EQ(orders.marketCalls, 0);
}

TEST(SignalEngineTest, GeminiEnforceComponentErrorBlocksOrderPlacement) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 9500 + i;
        k.high = 110.0;
        k.low = 90.0;
        k.close = 100.0;
        scanner.push("BTCUSDT", "4h", k);
    }

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    MockExposurePort exposure;
    MockGeminiFilterPort gemini;
    gemini.nextResult = {
        .decision = engine::GeminiDecision::Block,
        .confidence = 0.0,
        .sentimentScore = 0.0,
        .visionScore = 0.0,
        .reason = "Gemini component failure",
        .errorCode = "component_error",
        .hasError = true,
    };

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"4h"};
    cfg.minConfidence = 0.1;
    auto strategy = std::make_unique<MockStrategy>(cfg);
    auto* strategyPtr = strategy.get();
    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 1.0,
        .atr = 5.0,
    };
    registry.add(std::move(strategy));

    engine::GeminiFilterConfig geminiCfg;
    geminiCfg.enabled = true;
    geminiCfg.mode = engine::GeminiFilterMode::Enforce;
    geminiCfg.blockOnError = true;
    geminiCfg.maxEvaluationsPerScanCycle = 3;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, gemini, geminiCfg, {});
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr, "4h")));

    EXPECT_EQ(gemini.evaluateCalls, 1);
    EXPECT_EQ(orders.marketCalls, 0);
}

TEST(SignalEngineTest, GeminiComponentErrorCanFailOpenWhenConfigured) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 9550 + i;
        k.high = 110.0;
        k.low = 90.0;
        k.close = 100.0;
        scanner.push("BTCUSDT", "4h", k);
    }

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    MockExposurePort exposure;
    MockGeminiFilterPort gemini;
    gemini.nextResult = {
        .decision = engine::GeminiDecision::Block,
        .confidence = 0.0,
        .sentimentScore = 0.0,
        .visionScore = 0.0,
        .reason = "Gemini component failure",
        .errorCode = "component_error",
        .hasError = true,
    };

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"4h"};
    cfg.minConfidence = 0.1;
    auto strategy = std::make_unique<MockStrategy>(cfg);
    auto* strategyPtr = strategy.get();
    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 1.0,
        .atr = 5.0,
    };
    registry.add(std::move(strategy));

    engine::GeminiFilterConfig geminiCfg;
    geminiCfg.enabled = true;
    geminiCfg.mode = engine::GeminiFilterMode::Enforce;
    geminiCfg.blockOnError = false;
    geminiCfg.maxEvaluationsPerScanCycle = 3;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, gemini, geminiCfg, {});
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr, "4h")));

    EXPECT_EQ(gemini.evaluateCalls, 1);
    EXPECT_EQ(orders.marketCalls, 1);
}

TEST(SignalEngineTest, GeminiBudgetEnforceBlocksBeforeEvaluateByDefault) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 9700 + i;
        k.high = 110.0;
        k.low = 90.0;
        k.close = 100.0;
        scanner.push("BTCUSDT", "30m", k);
    }

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    MockExposurePort exposure;
    MockGeminiFilterPort gemini;

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"30m"};
    cfg.minConfidence = 0.1;
    auto strategy = std::make_unique<MockStrategy>(cfg);
    auto* strategyPtr = strategy.get();
    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 1.0,
        .atr = 5.0,
    };
    registry.add(std::move(strategy));

    engine::GeminiFilterConfig geminiCfg;
    geminiCfg.enabled = true;
    geminiCfg.mode = engine::GeminiFilterMode::Enforce;
    geminiCfg.maxEvaluationsPerScanCycle = 0;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, gemini, geminiCfg, {});
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr, "30m")));

    EXPECT_EQ(gemini.evaluateCalls, 0);
    EXPECT_EQ(orders.marketCalls, 0);
}

TEST(SignalEngineTest, GeminiDisabledDoesNotEvaluateAndAllowsOrderPlacement) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 9800 + i;
        k.high = 110.0;
        k.low = 90.0;
        k.close = 100.0;
        scanner.push("BTCUSDT", "30m", k);
    }

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    MockExposurePort exposure;
    MockGeminiFilterPort gemini;
    gemini.nextResult = {
        .decision = engine::GeminiDecision::Block,
        .confidence = 0.0,
        .sentimentScore = 0.0,
        .visionScore = 0.0,
        .reason = "should not be evaluated",
        .errorCode = "component_error",
        .hasError = true,
    };

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"30m"};
    cfg.minConfidence = 0.1;
    auto strategy = std::make_unique<MockStrategy>(cfg);
    auto* strategyPtr = strategy.get();
    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 1.0,
        .atr = 5.0,
    };
    registry.add(std::move(strategy));

    engine::GeminiFilterConfig geminiCfg;
    geminiCfg.enabled = false;
    geminiCfg.mode = engine::GeminiFilterMode::Disabled;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, gemini, geminiCfg, {});
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr, "30m")));

    EXPECT_EQ(gemini.evaluateCalls, 0);
    EXPECT_EQ(orders.marketCalls, 1);
}

TEST(SignalEngineTest, GeminiBudgetEnforceBlocksBeforeEvaluate) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline k;
        k.openTime = 10000 + i;
        k.high = 110.0;
        k.low = 90.0;
        k.close = 100.0;
        scanner.push("BTCUSDT", "30m", k);
    }

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    MockExposurePort exposure;
    MockGeminiFilterPort gemini;

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"30m"};
    cfg.minConfidence = 0.1;
    auto strategy = std::make_unique<MockStrategy>(cfg);
    auto* strategyPtr = strategy.get();
    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 1.0,
        .atr = 5.0,
    };
    registry.add(std::move(strategy));

    engine::GeminiFilterConfig geminiCfg;
    geminiCfg.enabled = true;
    geminiCfg.mode = engine::GeminiFilterMode::Enforce;
    geminiCfg.maxEvaluationsPerScanCycle = 0;
    engine::SignalEngine engine(scanner, registry, account, orders, exposure, gemini, geminiCfg, {});
    runAwaitable(ioc, engine.processItem(singleWork(strategyPtr, "30m")));

    EXPECT_EQ(gemini.evaluateCalls, 0);
    EXPECT_EQ(orders.marketCalls, 0);
}

TEST(SignalEngineTest, GeminiBudgetEnforceClosesGateForFollowingItemsInCycle) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({"BTCUSDT", "ETHUSDT"});
    scanner.setStep("BTCUSDT", 0.001);
    scanner.setStep("ETHUSDT", 0.001);
    for (int i = 0; i < 20; ++i) {
        Kline btc;
        btc.openTime = 11000 + i;
        btc.high = 110.0;
        btc.low = 90.0;
        btc.close = 100.0;
        scanner.push("BTCUSDT", "30m", btc);

        Kline eth;
        eth.openTime = 12000 + i;
        eth.high = 55.0;
        eth.low = 45.0;
        eth.close = 50.0;
        scanner.push("ETHUSDT", "30m", eth);
    }

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    MockExposurePort exposure;
    MockGeminiFilterPort gemini;

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"30m"};
    cfg.minConfidence = 0.1;
    auto strategy = std::make_unique<MockStrategy>(cfg);
    auto* strategyPtr = strategy.get();
    strategyPtr->nextSignal = strategy::Signal{
        .direction = strategy::Signal::Direction::Long,
        .confidence = 1.0,
        .atr = 5.0,
    };
    registry.add(std::move(strategy));

    engine::GeminiFilterConfig geminiCfg;
    geminiCfg.enabled = true;
    geminiCfg.mode = engine::GeminiFilterMode::Enforce;
    geminiCfg.maxEvaluationsPerScanCycle = 0;
    geminiCfg.closeGateOnBudgetExhausted = true;
    engine::SignalEngine signalEngine(scanner, registry, account, orders, exposure, gemini, geminiCfg, {});

    runAwaitable(ioc, signalEngine.processItem(singleWork(strategyPtr, "30m")));
    EXPECT_EQ(strategyPtr->evaluateCalls, 1);
    EXPECT_EQ(gemini.evaluateCalls, 0);
    EXPECT_EQ(orders.marketCalls, 0);

    ioc.restart();
    runAwaitable(
        ioc,
        signalEngine.processItem(engine::WorkItem{
            .symbol = "ETHUSDT",
            .interval = "30m",
            .strategy = strategyPtr,
        }));

    EXPECT_EQ(strategyPtr->evaluateCalls, 1);
    EXPECT_EQ(gemini.evaluateCalls, 0);
    EXPECT_EQ(orders.marketCalls, 0);
}

TEST(SignalEngineTest, RunScanCycleCallsRiskOnScanCycleAndMaybeRecomputeOncePerCycle) {
    boost::asio::io_context ioc;
    MockScannerPort scanner(ioc);
    scanner.setSymbols({});

    MockAccountPort account;
    account::AccountSnapshot snapshot;
    snapshot.account.availableBalance = 1000.0;
    account.nextSnapshot = snapshot;

    MockOrdersPort orders;
    MockExposurePort exposure;
    MockRiskPort risk;

    strategy::StrategyRegistry registry;
    strategy::StrategyConfig cfg;
    cfg.name = "mock";
    cfg.intervals = {"1h"};
    cfg.scanInterval = std::chrono::seconds{0};
    auto strategy = std::make_unique<MockStrategy>(cfg);
    registry.add(std::move(strategy));

    engine::SignalEngine signalEngine(scanner, registry, account, orders, exposure, {}, &risk);
    runAwaitable(ioc, signalEngine.runScanCycle());

    EXPECT_EQ(risk.onScanCycleCalls, 1);
    EXPECT_EQ(risk.maybeRecomputeCalls, 1);
    EXPECT_GT(risk.lastOnScanTs, 0);
    EXPECT_GT(risk.lastRecomputeTs, 0);
    EXPECT_GE(account.calls, 2);
}
