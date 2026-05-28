// Gate integration tests for SignalEngine — verifies that BacktestGate results
// (pass / drop / shadow / disabled) correctly gate or allow order placement.
//
// These tests exercise the path:
//   openPosition → preflightOpenPosition → evaluateBacktestNonBlocking → openPositionFromPreflight
//
// and the gate-bypass paths (disabled, shadow, budget-exhausted, non-eligible type).

#include <gtest/gtest.h>

#include "backtest/backtest_gate.h"
#include "engine/signal_engine.h"
#include "strategy/strategy_registry.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

// ── Async runner ──────────────────────────────────────────────────────────────

template <typename T>
T runAwaitable(boost::asio::io_context& ioc, boost::asio::awaitable<T> task) {
    auto fut = boost::asio::co_spawn(ioc, std::move(task), boost::asio::use_future);
    ioc.restart();
    ioc.run();
    return fut.get();
}

// ── Port stubs ────────────────────────────────────────────────────────────────

class ScannerStub final : public engine::IScannerPort {
public:
    explicit ScannerStub(boost::asio::io_context& ioc) : m_ioc(ioc), m_cache(200) {}
    const scanner::KlineCache& cache() const override { return m_cache; }
    std::vector<std::string> symbols() const override { return {}; }
    std::optional<ExchangeSymbol> symbolInfo(std::string_view sym) const override {
        ExchangeSymbol meta;
        meta.symbol = std::string(sym);
        meta.stepSize  = 0.001;
        meta.tickSize  = 0.01;
        meta.minNotional = 1.0;
        return meta;
    }
    boost::asio::io_context& ioContext() override { return m_ioc; }
private:
    boost::asio::io_context& m_ioc;
    scanner::KlineCache m_cache;
};

class AccountStub final : public engine::IAccountPort {
public:
    account::AccountServiceResult<account::AccountSnapshot> nextSnapshot =
        account::AccountSnapshot{};
    int calls{0};
    boost::asio::awaitable<account::AccountServiceResult<account::AccountSnapshot>> snapshot(
        account::AccountSnapshotRequest) override {
        ++calls;
        auto snap = nextSnapshot;
        co_return snap;
    }
};

class OrdersStub final : public engine::IOrdersPort {
public:
    int openNormalOrdersCalls{0};
    int marketCalls{0};
    boost::asio::awaitable<OrdersResult<std::vector<NormalOrderSnapshot>>>
    openNormalOrders(std::optional<Symbol>) override {
        ++openNormalOrdersCalls;
        co_return std::vector<NormalOrderSnapshot>{};
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> market(MarketOrderDraft) override {
        ++marketCalls;
        NormalPlacementResult r;
        r.state = PlacementState::Accepted;
        r.orderId = 1;
        r.avgPrice = "100";
        co_return OrdersResult<NormalPlacementResult>(r);
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> limit(LimitOrderDraft) override {
        co_return NormalPlacementResult{};
    }
    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> amendLimitOrder(
        AmendLimitOrderDraft) override {
        co_return NormalOrderSnapshot{};
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> protection(
        ProtectionOrderDraft) override {
        co_return NormalPlacementResult{};
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> closeByMarket(
        CloseByMarketDraft) override {
        co_return NormalPlacementResult{};
    }
    boost::asio::awaitable<OrdersResult<LeverageResult>> setLeverage(
        Symbol sym, int lev) override {
        co_return LeverageResult{.symbol = std::move(sym), .leverage = lev};
    }
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelNormalByOrderId(
        Symbol, int64_t) override { co_return NormalCancelResult{}; }
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelNormalByClientOrderId(
        Symbol, ClientOrderId) override { co_return NormalCancelResult{}; }
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelAlgoByAlgoId(
        Symbol, int64_t) override { co_return NormalCancelResult{}; }
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelAlgoByClientAlgoId(
        Symbol, ClientAlgoId) override { co_return NormalCancelResult{}; }
};

class OrderCapAllow final : public engine::IOrderCapPort {
public:
    mutable int calls{0};
    int blockFromCall{0};

    engine::OrderCapResult check(double, const account::AccountSnapshot&,
                                 const engine::PositionTracker&) const override {
        ++calls;
        if (blockFromCall > 0 && calls >= blockFromCall) {
            return {engine::OrderCapDecision::Block, "synthetic cap reached"};
        }
        return {engine::OrderCapDecision::Allow, "ok"};
    }
    engine::OrderCapFailureMode failureMode() const override {
        return engine::OrderCapFailureMode::Closed;
    }
};

class ExposureAllow final : public engine::IExposurePort {
public:
    engine::ExposureCheckResult check(std::string_view, strategy::Signal::Direction, double,
                                      const engine::PositionTracker&,
                                      const account::AccountSnapshot&,
                                      double) const override {
        return {engine::ExposureDecision::Allow, 1.0, "ok"};
    }
    engine::ExposureMetrics currentMetrics(const engine::PositionTracker&,
                                           const account::AccountSnapshot&,
                                           double) const override { return {}; }
    engine::ExposureFailureMode failureMode() const override {
        return engine::ExposureFailureMode::Open;
    }
    double minNotionalAfterScale() const override { return 0.0; }
};

// ── Fake backtest gate ────────────────────────────────────────────────────────

class FakeBacktestGate final : public backtest::IBacktestGatePort {
public:
    backtest::BacktestGateResult nextResult = backtest::PassResult{
        .direction       = strategy::Signal::Direction::Long,
        .atr             = 5.0,
        .initialStopPrice = 95.0,
        .slMultiplier    = 1.5,
        .tpMultiplier    = 3.0,
        .riskPct         = 0.01,
        .optimizedParams = {{"atr_period", 14}, {"sl_multiplier", 1.5}, {"tp_multiplier", 3.0}},
        .plateauVotePass = 3,
        .plateauVoteTotal = 4,
        .combosEvaluated = 42,
    };
    mutable int calls{0};

    backtest::BacktestGateResult evaluate(const backtest::BacktestGateRequest&) const override {
        ++calls;
        return nextResult;
    }
};

// ── Helpers ───────────────────────────────────────────────────────────────────

strategy::StrategyConfig eligibleCfg() {
    strategy::StrategyConfig cfg;
    cfg.name        = "golden_crossover_test";
    cfg.type        = "golden_crossover";  // whitelisted in isBacktestEligible()
    cfg.riskPct     = 0.01;
    cfg.slMultiplier = 1.5;
    cfg.tpMultiplier = 3.0;
    cfg.leverage    = 5;
    cfg.minNotional = 1.0;
    cfg.atrPeriod   = 14;
    cfg.minConfidence = 0.0;
    return cfg;
}

strategy::StrategyConfig nonEligibleCfg() {
    auto cfg = eligibleCfg();
    cfg.type = "trend_breakout";  // NOT whitelisted
    return cfg;
}

backtest::BacktestGateConfig enforceGateConfig() {
    backtest::BacktestGateConfig c;
    c.enabled    = true;
    c.shadowOnly = false;
    c.maxEvaluationsPerScanCycle = 10;
    return c;
}

backtest::BacktestGateConfig shadowGateConfig() {
    auto c = enforceGateConfig();
    c.shadowOnly = true;
    return c;
}

backtest::BacktestGateConfig disabledGateConfig() {
    auto c = enforceGateConfig();
    c.enabled = false;
    return c;
}

// Build a SignalEngine with all stubs wired.
struct TestHarness {
    boost::asio::io_context ioc;
    ScannerStub scanner{ioc};
    AccountStub account;
    OrdersStub orders;
    OrderCapAllow orderCap;
    ExposureAllow exposure;
    strategy::StrategyRegistry registry;
    engine::SignalEngine::Config engCfg{.placeStopLoss = false};
    std::unique_ptr<engine::SignalEngine> engine;

    TestHarness() {
        account.nextSnapshot = [] {
            account::AccountSnapshot s;
            s.account.availableBalance = 10000.0;
            return s;
        }();
        engine = std::make_unique<engine::SignalEngine>(
            scanner, registry, account, orders,
            orderCap, exposure, engCfg);
    }

    void openLong(strategy::StrategyConfig cfg = eligibleCfg()) {
        auto res = runAwaitable(ioc, engine->openPosition(
            "BTCUSDT", "1h",
            strategy::Signal::Direction::Long,
            /*atr=*/5.0, /*currentPrice=*/100.0,
            cfg, "test_signal"));
        (void)res;
    }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

// 1. Gate disabled → passthrough, market order placed, gate never called.
TEST(SignalEngineGateTest, DisabledGateNeverCalled) {
    TestHarness h;
    FakeBacktestGate gate;
    h.engine->setBacktestGate(&gate, disabledGateConfig());

    h.openLong();

    EXPECT_EQ(gate.calls, 0);
    EXPECT_EQ(h.orders.marketCalls, 1);
}

// 2. Enforce mode + gate pass → order placed, gate called once.
TEST(SignalEngineGateTest, EnforceModePassAllowsOrder) {
    TestHarness h;
    FakeBacktestGate gate;
    gate.nextResult = backtest::PassResult{
        .direction       = strategy::Signal::Direction::Long,
        .atr             = 8.0,
        .initialStopPrice = 92.0,
        .slMultiplier    = 2.0,
        .tpMultiplier    = 4.0,
        .riskPct         = 0.01,
        .plateauVotePass = 4,
        .plateauVoteTotal = 5,
        .combosEvaluated = 100,
    };
    h.engine->setBacktestGate(&gate, enforceGateConfig());

    h.openLong();

    EXPECT_EQ(gate.calls, 1);
    EXPECT_EQ(h.orders.marketCalls, 1);
}

TEST(SignalEngineGateTest, GatePassRevalidatesMutableLiveGuardsBeforeOrder) {
    TestHarness h;
    FakeBacktestGate gate;
    gate.nextResult = backtest::PassResult{
        .direction       = strategy::Signal::Direction::Long,
        .atr             = 8.0,
        .initialStopPrice = 92.0,
        .slMultiplier    = 2.0,
        .tpMultiplier    = 4.0,
        .riskPct         = 0.01,
        .plateauVotePass = 4,
        .plateauVoteTotal = 5,
        .combosEvaluated = 100,
    };
    h.orderCap.blockFromCall = 2;  // initial preflight allows; post-gate revalidation blocks
    h.engine->setBacktestGate(&gate, enforceGateConfig());

    h.openLong();

    EXPECT_EQ(gate.calls, 1);
    EXPECT_EQ(h.orderCap.calls, 2);
    EXPECT_EQ(h.orders.marketCalls, 0);
}

// 3. Enforce mode + gate drop → order blocked.
TEST(SignalEngineGateTest, EnforceModeDropBlocksOrder) {
    TestHarness h;
    FakeBacktestGate gate;
    gate.nextResult = backtest::DropDetail{
        .reason  = backtest::DropReason::NoPlateauFound,
        .message = "synthetic drop",
        .combosEvaluated = 200,
    };
    h.engine->setBacktestGate(&gate, enforceGateConfig());

    h.openLong();

    EXPECT_EQ(gate.calls, 1);
    EXPECT_EQ(h.orders.marketCalls, 0);
}

// 4. Shadow mode + gate drop → order STILL placed (shadow-only never blocks).
TEST(SignalEngineGateTest, ShadowModeDropStillOpensOrder) {
    TestHarness h;
    FakeBacktestGate gate;
    gate.nextResult = backtest::DropDetail{
        .reason  = backtest::DropReason::MajorityVoteFailed,
        .message = "shadow drop",
        .combosEvaluated = 50,
    };
    h.engine->setBacktestGate(&gate, shadowGateConfig());

    h.openLong();

    EXPECT_EQ(gate.calls, 1);
    EXPECT_EQ(h.orders.marketCalls, 1);  // shadow never blocks
}

// 5. Non-eligible strategy type → gate is never evaluated.
TEST(SignalEngineGateTest, NonEligibleStrategyTypeSkipsGate) {
    TestHarness h;
    FakeBacktestGate gate;
    h.engine->setBacktestGate(&gate, enforceGateConfig());

    h.openLong(nonEligibleCfg());

    EXPECT_EQ(gate.calls, 0);
    EXPECT_EQ(h.orders.marketCalls, 1);  // gate skipped → order placed
}

// 6. Per-scan budget exhaustion: after maxEvaluationsPerScanCycle calls in enforce
//    mode, subsequent eligible signals in the same cycle are blocked without
//    calling the gate.
TEST(SignalEngineGateTest, BudgetExhaustedBlocksRemainingInCycle) {
    TestHarness h;
    FakeBacktestGate gate;
    // Gate always passes.
    gate.nextResult = backtest::PassResult{
        .direction       = strategy::Signal::Direction::Long,
        .atr             = 5.0,
        .initialStopPrice = 95.0,
        .slMultiplier    = 1.5,
        .tpMultiplier    = 3.0,
        .plateauVotePass = 3,
        .plateauVoteTotal = 4,
        .combosEvaluated = 10,
    };

    auto cfg = enforceGateConfig();
    cfg.maxEvaluationsPerScanCycle = 1;
    cfg.closeGateOnBudgetExhausted = false;  // don't close gate, just block extras
    h.engine->setBacktestGate(&gate, cfg);

    // First call: consumes the budget.
    auto res1 = runAwaitable(h.ioc, h.engine->openPosition(
        "BTCUSDT", "1h", strategy::Signal::Direction::Long,
        5.0, 100.0, eligibleCfg(), "first"));
    (void)res1;

    // Second call: budget exhausted → gate not called, order blocked in enforce mode.
    auto res2 = runAwaitable(h.ioc, h.engine->openPosition(
        "BTCUSDT", "1h", strategy::Signal::Direction::Long,
        5.0, 100.0, eligibleCfg(), "second"));
    (void)res2;

    EXPECT_EQ(gate.calls, 1);        // gate only called for the first evaluation
    EXPECT_EQ(h.orders.marketCalls, 1);  // only first order placed
}

// 7. Preflight failure (zero balance → sizing yields 0 qty) → gate is never called.
TEST(SignalEngineGateTest, PreflightFailSkipsGate) {
    TestHarness h;
    // Override account to return zero balance → sizing will fail.
    account::AccountSnapshot zeroSnap;
    zeroSnap.account.availableBalance = 0.0;
    h.account.nextSnapshot = zeroSnap;

    FakeBacktestGate gate;
    h.engine->setBacktestGate(&gate, enforceGateConfig());

    h.openLong();

    // Preflight fails at sizing → gate is never reached.
    EXPECT_EQ(gate.calls, 0);
    EXPECT_EQ(h.orders.marketCalls, 0);
}

}  // namespace
