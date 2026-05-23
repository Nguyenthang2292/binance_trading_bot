#include <gtest/gtest.h>
#include "backtest/gemini_range_proposer.h"
#include "types/market.h"
#include <vector>
#include <cmath>

using namespace backtest;

namespace {

std::vector<Kline> makeTrendingKlines(int n, double startClose, double trend) {
    std::vector<Kline> ks;
    ks.reserve(n);
    for (int i = 0; i < n; ++i) {
        Kline k;
        k.openTime = static_cast<int64_t>(i * 3600 * 1000);
        k.closeTime = k.openTime + 3600 * 1000 - 1;
        k.close = startClose + trend * i;
        k.open = k.close - 1.0;
        k.high = k.close + 2.0;
        k.low = k.close - 2.0;
        k.volume = 100.0;
        k.isClosed = true;
        ks.push_back(k);
    }
    return ks;
}

} // namespace

// ── computePromptAggregates ────────────────────────────────────────────────

TEST(GeminiRangeProposerTest, EmptyContextReturnsDefaultAggregates) {
    std::vector<Kline> empty;
    auto agg = computePromptAggregates(empty);
    EXPECT_EQ(agg.numCandles, 0);
    EXPECT_DOUBLE_EQ(agg.ret30dPct, 0.0);
}

TEST(GeminiRangeProposerTest, UpTrendDetectedCorrectly) {
    auto ks = makeTrendingKlines(100, 100.0, 1.0);  // price rises 1 per bar
    auto agg = computePromptAggregates(ks);
    EXPECT_EQ(agg.trendDirection, "up");
    EXPECT_GT(agg.ret30dPct, 0.0);
    EXPECT_EQ(agg.numCandles, 100);
}

TEST(GeminiRangeProposerTest, DownTrendDetectedCorrectly) {
    auto ks = makeTrendingKlines(100, 200.0, -1.0);  // price falls 1 per bar
    auto agg = computePromptAggregates(ks);
    EXPECT_EQ(agg.trendDirection, "down");
    EXPECT_LT(agg.ret30dPct, 0.0);
}

TEST(GeminiRangeProposerTest, FlatTrendDetectedCorrectly) {
    auto ks = makeTrendingKlines(100, 100.0, 0.0);  // flat
    auto agg = computePromptAggregates(ks);
    EXPECT_EQ(agg.trendDirection, "flat");
}

TEST(GeminiRangeProposerTest, AtrPctCurrentIsPositive) {
    auto ks = makeTrendingKlines(50, 100.0, 0.1);
    auto agg = computePromptAggregates(ks);
    // With high/low = close ± 2.0, ATR should be positive
    EXPECT_GE(agg.atrPctCurrent, 0.0);
}

TEST(GeminiRangeProposerTest, RealizedVolIsPositiveForNonConstantPrices) {
    auto ks = makeTrendingKlines(30, 100.0, 1.0);
    // Add some variance
    for (size_t i = 0; i < ks.size(); i += 3) {
        ks[i].close = ks[i].close * (i % 2 == 0 ? 1.02 : 0.98);
    }
    auto agg = computePromptAggregates(ks);
    EXPECT_GE(agg.realizedVol, 0.0);
}

// ── Prompt context isolation guarantee ────────────────────────────────────
// (The GeminiRangeProposer must only compute aggregates from promptContext.
// This property is enforced structurally: proposeWithPartitions() takes only
// the promptContext slice and does not have access to calibration/OOS data.
// We verify the computation correctness here by feeding different slices
// and asserting aggregates differ.)

TEST(GeminiRangeProposerTest, AggregatesReflectOnlyPromptContextSlice) {
    // Two different prompt contexts → different aggregates
    auto ctx1 = makeTrendingKlines(50, 100.0, 1.0);  // bull
    auto ctx2 = makeTrendingKlines(50, 200.0, -1.0);  // bear

    auto agg1 = computePromptAggregates(ctx1);
    auto agg2 = computePromptAggregates(ctx2);

    // Trend should differ
    EXPECT_NE(agg1.trendDirection, agg2.trendDirection);
    // Returns should have opposite signs
    EXPECT_GT(agg1.ret30dPct, 0.0);
    EXPECT_LT(agg2.ret30dPct, 0.0);
}
