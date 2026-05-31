// Tests for BacktestEngine — focused on correctness primitives, NOT plugin parity.
// Uses a hand-written deterministic IOptimizableStrategy stub to drive exits.
//
// Engine timeline (matches BacktestEngine::runFold loop):
//   i=0: push klines[0]; eval signal → enter at klines[1].open
//   i=1: push klines[1]; check exit on klines[2]   ← TP/SL evaluated here
//   ...
// So a 3-bar setup is the minimum to test exit logic.

#include "backtest/backtest_engine.h"
#include "backtest/optimizable_strategy.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace backtest;

namespace {

class AlwaysLongStub : public IOptimizableStrategy {
public:
    explicit AlwaysLongStub(double atr) : m_atr(atr) {}
    StrategyParamSpec spec(const strategy::StrategyConfig&) const override { return {}; }
    strategy::Signal evaluateWith(std::string_view, std::string_view,
                                  const std::vector<Kline>&,
                                  const ParamPoint&,
                                  const strategy::StrategyConfig&) const override {
        strategy::Signal s;
        s.direction = strategy::Signal::Direction::Long;
        s.confidence = 1.0;
        s.atr = m_atr;
        return s;
    }
private:
    double m_atr;
};

class AlwaysShortStub : public IOptimizableStrategy {
public:
    explicit AlwaysShortStub(double atr) : m_atr(atr) {}
    StrategyParamSpec spec(const strategy::StrategyConfig&) const override { return {}; }
    strategy::Signal evaluateWith(std::string_view, std::string_view,
                                  const std::vector<Kline>&,
                                  const ParamPoint&,
                                  const strategy::StrategyConfig&) const override {
        strategy::Signal s;
        s.direction = strategy::Signal::Direction::Short;
        s.confidence = 1.0;
        s.atr = m_atr;
        return s;
    }
private:
    double m_atr;
};

Kline makeBar(int64_t openTimeMs, double open, double high, double low, double close) {
    Kline k{};
    k.openTime = openTimeMs;
    k.closeTime = openTimeMs + 60'000;
    k.open = open;
    k.high = high;
    k.low = low;
    k.close = close;
    k.isClosed = true;
    return k;
}

strategy::StrategyConfig makeBaseCfg() {
    strategy::StrategyConfig cfg;
    cfg.atrPeriod = 14;
    cfg.minConfidence = 0.0;
    cfg.slMultiplier = 1.0;
    cfg.tpMultiplier = 2.0;
    cfg.leverage = 1;
    cfg.maxHoldDuration = std::chrono::seconds(86400);
    cfg.takeProfitPercent = 0.0;
    return cfg;
}

ExchangeSymbol makeSymbolMeta(double tickSize) {
    ExchangeSymbol meta;
    meta.symbol = "X";
    meta.tickSize = tickSize;
    return meta;
}

}  // namespace

TEST(BacktestEngineTest, NoTradesOnEmptyOrSingleBar) {
    BacktestEngine engine(BacktestEngine::Config{});
    AlwaysLongStub stub(1.0);
    auto cfg = makeBaseCfg();

    EXPECT_EQ(engine.runFold(stub, "X", "1m", {}, {}, cfg,
                             strategy::Signal::Direction::Long).numTrades, 0);
    std::vector<Kline> oneBar{makeBar(0, 100, 101, 99, 100)};
    EXPECT_EQ(engine.runFold(stub, "X", "1m", oneBar, {}, cfg,
                             strategy::Signal::Direction::Long).numTrades, 0);
}

TEST(BacktestEngineTest, LongTakeProfit) {
    BacktestEngine engine(BacktestEngine::Config{0.0, 0.0, false});
    AlwaysLongStub stub(/*atr=*/10.0);
    auto cfg = makeBaseCfg();   // SL = entry-10, TP = entry+20

    // bar0: signal observed.
    // bar1: entry at open=100 → SL=90, TP=120.
    // bar2: high=125 crosses TP=120 → exit.
    std::vector<Kline> klines{
        makeBar(0,        100, 100, 100, 100),
        makeBar(60'000,   100, 101, 99,  100),
        makeBar(120'000,  100, 125, 99.5, 120),
    };
    auto stats = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                strategy::Signal::Direction::Long);
    ASSERT_EQ(stats.numTrades, 1);
    EXPECT_EQ(stats.winRate, 1.0);
}

TEST(BacktestEngineTest, UsesFiniteRatiosForSingleSidedReturnDistributions) {
    BacktestEngine engine(BacktestEngine::Config{0.0, 0.0, false});
    AlwaysLongStub stub(/*atr=*/10.0);
    auto cfg = makeBaseCfg();

    std::vector<Kline> klines{
        makeBar(0,        100, 100, 100, 100),
        makeBar(60'000,   100, 101, 99,  100),
        makeBar(120'000,  100, 125, 99.5, 120),
    };
    const auto stats = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                      strategy::Signal::Direction::Long);
    ASSERT_EQ(stats.numTrades, 1);
    EXPECT_TRUE(std::isinf(stats.profitFactor));
    EXPECT_TRUE(std::isfinite(stats.sortino));
    EXPECT_TRUE(std::isfinite(stats.sharpe));
    EXPECT_DOUBLE_EQ(stats.sortino, 0.0);
    EXPECT_DOUBLE_EQ(stats.sharpe, 0.0);
}

TEST(BacktestEngineTest, EntryCandleStopIsAppliedConservatively) {
    BacktestEngine engine(BacktestEngine::Config{0.0, 0.0, false});
    AlwaysLongStub stub(/*atr=*/10.0);
    auto cfg = makeBaseCfg();

    std::vector<Kline> klines{
        makeBar(0,       100, 100, 100, 100),
        makeBar(60'000,  100, 125,  85,  95),  // entry candle hits both SL=90 and TP=120
        makeBar(120'000, 100, 125,  99, 120),
    };

    const auto stats = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                      strategy::Signal::Direction::Long);
    ASSERT_EQ(stats.numTrades, 1);
    EXPECT_DOUBLE_EQ(stats.winRate, 0.0);
}

TEST(BacktestEngineTest, OpenPositionIsMarkedToMarketAtFoldEnd) {
    BacktestEngine engine(BacktestEngine::Config{0.0, 0.0, false});
    AlwaysLongStub stub(/*atr=*/50.0);
    auto cfg = makeBaseCfg();

    std::vector<Kline> klines{
        makeBar(0,       100, 100, 100, 100),
        makeBar(60'000,  100, 101,  99, 105),
        makeBar(120'000, 105, 106, 104, 106),
    };

    const auto stats = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                      strategy::Signal::Direction::Long);
    ASSERT_EQ(stats.numTrades, 1);
    EXPECT_DOUBLE_EQ(stats.winRate, 1.0);
}

TEST(BacktestEngineTest, RejectsDerivedNonPositiveExitPrices) {
    BacktestEngine engine(BacktestEngine::Config{0.0, 0.0, false});
    AlwaysShortStub stub(/*atr=*/200.0);
    auto cfg = makeBaseCfg();
    cfg.tpMultiplier = 1.0;  // short TP = entry - 200, invalid.

    std::vector<Kline> klines{
        makeBar(0,       100, 100, 100, 100),
        makeBar(60'000,  100, 110,  90,  95),
        makeBar(120'000,  95, 100,  90,  95),
    };

    const auto stats = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                      strategy::Signal::Direction::Short);
    EXPECT_EQ(stats.numTrades, 0);
}

TEST(BacktestEngineTest, UsesFixedTakeProfitPercentWhenConfiguredOnStrategy) {
    BacktestEngine engine(BacktestEngine::Config{0.0, 0.0, false});
    AlwaysLongStub stub(/*atr=*/10.0);
    auto cfg = makeBaseCfg();
    cfg.tpMultiplier = 20.0;       // ATR TP would be far away.
    cfg.takeProfitPercent = 10.0;  // Live path prefers fixed TP percent when set.

    std::vector<Kline> klines{
        makeBar(0,        100, 100, 100, 100),
        makeBar(60'000,   100, 101, 99,  100),
        makeBar(120'000,  100, 111, 99.5, 110),
    };
    auto stats = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                strategy::Signal::Direction::Long);
    ASSERT_EQ(stats.numTrades, 1);
    EXPECT_EQ(stats.winRate, 1.0);
}

TEST(BacktestEngineTest, TickRoundsLongTakeProfitDownLikeLiveOrders) {
    BacktestEngine engine(BacktestEngine::Config{0.0, 0.0, false});
    AlwaysLongStub stub(/*atr=*/10.2);
    auto cfg = makeBaseCfg();
    auto meta = makeSymbolMeta(/*tickSize=*/1.0);

    std::vector<Kline> klines{
        makeBar(0,        100, 100,   100, 100),
        makeBar(60'000,   100, 101,    99, 100),
        makeBar(120'000,  100, 120.1,  99, 100),
    };
    auto withoutMeta = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                      strategy::Signal::Direction::Long);
    auto withMeta = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                   strategy::Signal::Direction::Long, meta);

    ASSERT_EQ(withoutMeta.numTrades, 1);  // raw TP=120.4 is not touched; fold-end close is flat.
    EXPECT_EQ(withoutMeta.winRate, 0.0);
    ASSERT_EQ(withMeta.numTrades, 1);     // tick TP=120.0 is touched.
    EXPECT_EQ(withMeta.winRate, 1.0);
}

TEST(BacktestEngineTest, TickRoundsShortTakeProfitUpLikeLiveOrders) {
    BacktestEngine engine(BacktestEngine::Config{0.0, 0.0, false});
    AlwaysShortStub stub(/*atr=*/10.2);
    auto cfg = makeBaseCfg();
    auto meta = makeSymbolMeta(/*tickSize=*/1.0);

    std::vector<Kline> klines{
        makeBar(0,        100, 100,   100, 100),
        makeBar(60'000,   100, 101,    99, 100),
        makeBar(120'000,  100, 100.1, 79.9, 100),
    };
    auto withoutMeta = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                      strategy::Signal::Direction::Short);
    auto withMeta = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                   strategy::Signal::Direction::Short, meta);

    ASSERT_EQ(withoutMeta.numTrades, 1);  // raw TP=79.6 is not touched; fold-end close is flat.
    EXPECT_EQ(withoutMeta.winRate, 0.0);
    ASSERT_EQ(withMeta.numTrades, 1);     // tick TP=80.0 is touched.
    EXPECT_EQ(withMeta.winRate, 1.0);
}

TEST(BacktestEngineTest, TickRoundsLongStopLossUpLikeLiveOrders) {
    BacktestEngine engine(BacktestEngine::Config{0.0, 0.0, false});
    AlwaysLongStub stub(/*atr=*/10.2);
    auto cfg = makeBaseCfg();
    auto meta = makeSymbolMeta(/*tickSize=*/1.0);

    std::vector<Kline> klines{
        makeBar(0,        100, 100,  100, 100),
        makeBar(60'000,   100, 101,   99, 100),
        makeBar(120'000,  100, 100.1, 89.9, 100),
    };
    auto withoutMeta = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                      strategy::Signal::Direction::Long);
    auto withMeta = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                   strategy::Signal::Direction::Long, meta);

    ASSERT_EQ(withoutMeta.numTrades, 1);  // raw SL=89.8 is not touched; fold-end close is flat.
    EXPECT_EQ(withoutMeta.winRate, 0.0);
    ASSERT_EQ(withMeta.numTrades, 1);     // tick SL=90.0 is touched.
    EXPECT_EQ(withMeta.winRate, 0.0);
}

TEST(BacktestEngineTest, TickRoundsShortStopLossDownLikeLiveOrders) {
    BacktestEngine engine(BacktestEngine::Config{0.0, 0.0, false});
    AlwaysShortStub stub(/*atr=*/10.2);
    auto cfg = makeBaseCfg();
    auto meta = makeSymbolMeta(/*tickSize=*/1.0);

    std::vector<Kline> klines{
        makeBar(0,        100, 100,   100, 100),
        makeBar(60'000,   100, 101,    99, 100),
        makeBar(120'000,  100, 110.1, 99.9, 100),
    };
    auto withoutMeta = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                      strategy::Signal::Direction::Short);
    auto withMeta = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                   strategy::Signal::Direction::Short, meta);

    ASSERT_EQ(withoutMeta.numTrades, 1);  // raw SL=110.2 is not touched; fold-end close is flat.
    EXPECT_EQ(withoutMeta.winRate, 0.0);
    ASSERT_EQ(withMeta.numTrades, 1);     // tick SL=110.0 is touched.
    EXPECT_EQ(withMeta.winRate, 0.0);
}

TEST(BacktestEngineTest, TickRoundsFixedTakeProfitPercent) {
    BacktestEngine engine(BacktestEngine::Config{0.0, 0.0, false});
    AlwaysLongStub stub(/*atr=*/10.0);
    auto cfg = makeBaseCfg();
    cfg.tpMultiplier = 20.0;
    cfg.takeProfitPercent = 20.4;
    auto meta = makeSymbolMeta(/*tickSize=*/1.0);

    std::vector<Kline> klines{
        makeBar(0,        100, 100,   100, 100),
        makeBar(60'000,   100, 101,    99, 100),
        makeBar(120'000,  100, 120.1,  99, 100),
    };
    auto withoutMeta = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                      strategy::Signal::Direction::Long);
    auto withMeta = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                   strategy::Signal::Direction::Long, meta);

    ASSERT_EQ(withoutMeta.numTrades, 1);  // raw fixed TP=120.4 is not touched; fold-end close is flat.
    EXPECT_EQ(withoutMeta.winRate, 0.0);
    ASSERT_EQ(withMeta.numTrades, 1);     // tick fixed TP=120.0 is touched.
    EXPECT_EQ(withMeta.winRate, 1.0);
}

TEST(BacktestEngineTest, InvalidTickSizeKeepsUnroundedBehaviour) {
    BacktestEngine engine(BacktestEngine::Config{0.0, 0.0, false});
    AlwaysLongStub stub(/*atr=*/10.2);
    auto cfg = makeBaseCfg();
    auto meta = makeSymbolMeta(/*tickSize=*/0.0);

    std::vector<Kline> klines{
        makeBar(0,        100, 100,   100, 100),
        makeBar(60'000,   100, 101,    99, 100),
        makeBar(120'000,  100, 120.1,  99, 100),
    };
    auto stats = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                strategy::Signal::Direction::Long, meta);

    ASSERT_EQ(stats.numTrades, 1);
    EXPECT_EQ(stats.winRate, 0.0);
}

TEST(BacktestEngineTest, LongStopLossWinsOnSameCandle) {
    // Conservative ordering: stop hits first when both SL and TP inside one bar.
    BacktestEngine engine(BacktestEngine::Config{0.0, 0.0, false});
    AlwaysLongStub stub(10.0);
    auto cfg = makeBaseCfg();

    // bar0: signal. bar1: entry@100. bar2: low=85 (SL=90 hit) AND high=125 (TP=120 hit).
    std::vector<Kline> klines{
        makeBar(0,        100, 100, 100, 100),
        makeBar(60'000,   100, 101,  99, 100),
        makeBar(120'000,  100, 125,  85,  95),
    };
    auto stats = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                strategy::Signal::Direction::Long);
    ASSERT_EQ(stats.numTrades, 1);
    EXPECT_EQ(stats.winRate, 0.0);  // loss — stop wins on collision
}

TEST(BacktestEngineTest, ShortSymmetricWithLong) {
    BacktestEngine engine(BacktestEngine::Config{0.0, 0.0, false});
    auto cfg = makeBaseCfg();

    AlwaysLongStub longStub(5.0);
    AlwaysShortStub shortStub(5.0);

    // Long: entry@100, TP=110, hit on bar2.
    std::vector<Kline> upTrend{
        makeBar(0,        100, 100, 100, 100),
        makeBar(60'000,   100, 101,  99, 100),
        makeBar(120'000,  100, 115,  99, 110),
    };
    // Short: entry@100, TP=90, hit on bar2.
    std::vector<Kline> downTrend{
        makeBar(0,        100, 100, 100, 100),
        makeBar(60'000,   100, 101,  99, 100),
        makeBar(120'000,  100, 101,  85,  90),
    };

    auto lStats = engine.runFold(longStub, "X", "1m", upTrend, {}, cfg,
                                 strategy::Signal::Direction::Long);
    auto sStats = engine.runFold(shortStub, "X", "1m", downTrend, {}, cfg,
                                 strategy::Signal::Direction::Short);

    EXPECT_EQ(lStats.numTrades, 1);
    EXPECT_EQ(sStats.numTrades, 1);
    EXPECT_EQ(lStats.winRate, sStats.winRate);
}

TEST(BacktestEngineTest, FeesIncreaseDrawdownOnLosingTrade) {
    // Use a losing trade — fees compound the loss so the with-fee engine
    // must report a larger maxDrawdown than the no-fee engine.
    BacktestEngine engineNoFee(BacktestEngine::Config{0.0,    0.0, false});
    BacktestEngine engineWithFee(BacktestEngine::Config{0.001, 0.0, false});
    AlwaysLongStub stub(10.0);
    auto cfg = makeBaseCfg();   // SL = entry - 10

    std::vector<Kline> klines{
        makeBar(0,        100, 100, 100, 100),
        makeBar(60'000,   100, 101,  99, 100),
        makeBar(120'000,  100, 101,  85,  90),   // low=85 → SL=90 hit
    };
    auto a = engineNoFee.runFold(stub, "X", "1m", klines, {}, cfg,
                                 strategy::Signal::Direction::Long);
    auto b = engineWithFee.runFold(stub, "X", "1m", klines, {}, cfg,
                                   strategy::Signal::Direction::Long);
    ASSERT_EQ(a.numTrades, 1);
    ASSERT_EQ(b.numTrades, 1);
    EXPECT_EQ(a.winRate, 0.0);
    EXPECT_EQ(b.winRate, 0.0);
    EXPECT_GT(b.maxDrawdown, a.maxDrawdown);
}

TEST(BacktestEngineTest, DeterministicAcrossRuns) {
    BacktestEngine engine(BacktestEngine::Config{0.0004, 0.0, false});
    AlwaysLongStub stub(5.0);
    auto cfg = makeBaseCfg();
    std::vector<Kline> klines{
        makeBar(0,        100, 100, 100, 100),
        makeBar(60'000,   100, 101,  99, 100),
        makeBar(120'000,  100, 115,  99, 110),
    };
    auto s1 = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                             strategy::Signal::Direction::Long);
    auto s2 = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                             strategy::Signal::Direction::Long);
    EXPECT_EQ(s1.numTrades, s2.numTrades);
    EXPECT_DOUBLE_EQ(s1.sortino, s2.sortino);
    EXPECT_DOUBLE_EQ(s1.profitFactor, s2.profitFactor);
}

TEST(BacktestEngineTest, RejectsSignalNotMatchingAcceptedDirection) {
    BacktestEngine engine(BacktestEngine::Config{});
    AlwaysShortStub stub(10.0);  // emits Short
    auto cfg = makeBaseCfg();

    std::vector<Kline> klines{
        makeBar(0,        100, 100, 100, 100),
        makeBar(60'000,   100, 121,  99, 110),
        makeBar(120'000,  100, 121,  99, 110),
    };
    // Accept only Long → no trades despite Short signal.
    auto stats = engine.runFold(stub, "X", "1m", klines, {}, cfg,
                                strategy::Signal::Direction::Long);
    EXPECT_EQ(stats.numTrades, 0);
}
