#include <gtest/gtest.h>
#include "backtest/gemini_range_proposer.h"
#include "types/market.h"
#include <chrono>
#include <vector>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace backtest;

namespace {

namespace fs = std::filesystem;

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

RangeProposalRequest makeRequest(const std::vector<Kline>& promptContext) {
    RangeProposalRequest req;
    req.symbol = "BTCUSDT";
    req.interval = "1h";
    req.strategyId = "golden_crossover";
    req.tunableParams = {"ma_short", "ma_long"};
    req.defaultRanges = {
        {"ma_short", 10.0, 40.0, 5.0, true},
        {"ma_long", 50.0, 200.0, 10.0, true},
    };
    req.constraints = {
        {"ma_short", ParamConstraint::Kind::LessThan, "ma_long"},
    };
    req.currentValues = {
        {"ma_short", 20.0},
        {"ma_long", 100.0},
    };
    req.signalDirection = strategy::Signal::Direction::Long;
    req.signalBarOpenTimeMs = 1'700'000'000'000LL;
    req.maxTotalCombos = 2000;
    req.promptContext = promptContext;
    req.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    return req;
}

int countEvalDirs(const fs::path& runtimeDir) {
    if (!fs::exists(runtimeDir) || !fs::is_directory(runtimeDir)) {
        return 0;
    }
    int count = 0;
    for (const auto& entry : fs::directory_iterator(runtimeDir)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.rfind("eval-", 0) == 0) {
            ++count;
        }
    }
    return count;
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

TEST(GeminiRangeProposerTest, SuccessfulProposalCleansEvalDirectory) {
    const auto uniqueSuffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path testRoot =
        fs::temp_directory_path() / ("gemini_range_proposer_cleanup_" + uniqueSuffix);
    struct TempDirCleanup {
        fs::path path;
        ~TempDirCleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{testRoot};

    ASSERT_TRUE(fs::create_directories(testRoot));
    const fs::path modulePath = testRoot / "fake_proposer_main.py";
    std::ofstream moduleFile(modulePath);
    ASSERT_TRUE(moduleFile.is_open());
    moduleFile
        << "import json\n"
        << "import sys\n"
        << "with open(sys.argv[2], 'w', encoding='utf-8') as fh:\n"
        << "    json.dump({\n"
        << "        'ranges': [\n"
        << "            {'name': 'ma_short', 'min': 10, 'max': 30, 'step': 5, 'is_integer': True},\n"
        << "            {'name': 'ma_long', 'min': 60, 'max': 140, 'step': 10, 'is_integer': True},\n"
        << "        ],\n"
        << "        'notes': 'ok'\n"
        << "    }, fh)\n";
    moduleFile.close();

    BacktestGateGeminiConfig cfg;
    cfg.pythonPath = "python";
    cfg.moduleName = "fake_proposer_main";
    cfg.workingDirectory = testRoot.string();
    cfg.runtimeDir = (testRoot / "runtime").string();
    cfg.timeoutSeconds = 5;

    GeminiRangeProposer proposer(cfg);
    const auto req = makeRequest(makeTrendingKlines(80, 100.0, 0.5));
    const auto result = proposer.propose(req);
    ASSERT_TRUE(std::holds_alternative<IRangeProposer::Output>(result));
    EXPECT_EQ(countEvalDirs(fs::path(cfg.runtimeDir)), 0);
}
