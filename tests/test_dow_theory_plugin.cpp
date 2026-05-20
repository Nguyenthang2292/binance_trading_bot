#include <gtest/gtest.h>

#include "catalog/plugin_handle.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char* kPluginFilename = "strategy_dow_theory.dll";

std::filesystem::path findPluginPathFrom(std::filesystem::path start) {
    if (start.empty()) {
        return {};
    }
    std::error_code ec;
    start = std::filesystem::absolute(start, ec);
    if (ec) {
        return {};
    }

    auto current = std::move(start);
    while (!current.empty()) {
        const auto candidate = current / "plugins" / kPluginFilename;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }
        for (const auto& config : {"Debug", "Release", "RelWithDebInfo"}) {
            const auto buildTreeCandidate = current / "plugins" / "src" / "dow_theory" / config / kPluginFilename;
            if (std::filesystem::exists(buildTreeCandidate, ec) && !ec) {
                return buildTreeCandidate;
            }
        }
        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

std::filesystem::path dowTheoryPluginPath() {
    if (const auto byCwd = findPluginPathFrom(std::filesystem::current_path()); !byCwd.empty()) {
        return byCwd;
    }
    if (const auto byFile = findPluginPathFrom(std::filesystem::path(__FILE__).parent_path()); !byFile.empty()) {
        return byFile;
    }
    for (const auto& config : {"Debug", "Release", "RelWithDebInfo"}) {
        const auto candidate = std::filesystem::path(BOT_SOURCE_DIR) / "build" / "bin" / config / "plugins" / kPluginFilename;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }
        const auto codexCandidate =
            std::filesystem::path(BOT_SOURCE_DIR) / "build_codex" / "plugins" / "src" / "dow_theory" / config / kPluginFilename;
        if (std::filesystem::exists(codexCandidate, ec) && !ec) {
            return codexCandidate;
        }
    }
    throw std::runtime_error("unable to locate strategy_dow_theory.dll");
}

catalog::PluginHandle loadDowTheoryPlugin() {
    auto loaded = catalog::PluginHandle::load(dowTheoryPluginPath());
    if (!loaded) {
        throw std::runtime_error(loaded.error());
    }
    return std::move(*loaded);
}

using StrategyPtr = std::unique_ptr<strategy::IStrategy, catalog::PluginHandle::DestroyFn>;

StrategyPtr createDowTheoryStrategy(catalog::PluginHandle& plugin) {
    const std::string cfg = R"json(
{
  "name": "Dow Theory",
  "type": "dow_theory",
  "intervals": ["30m"],
  "atr_period": 14,
  "min_confidence": 0.0,
  "takeProfitPercent": 20.0,
  "tp_multiplier": 3.0,
  "params": {
    "swing_atr_mult": 1.5
  }
}
)json";
    strategy::IStrategy* raw = plugin.create(cfg.c_str());
    if (!raw) {
        throw std::runtime_error("createStrategy returned nullptr");
    }
    return StrategyPtr(raw, plugin.destroyFunction());
}

void pushCandle(std::vector<Kline>& out, double close, double high, double low, bool isClosed = true) {
    Kline k;
    k.openTime = out.empty() ? 1000 : (out.back().openTime + 1);
    k.closeTime = k.openTime + 1;
    k.open = close;
    k.high = high;
    k.low = low;
    k.close = close;
    k.volume = 1.0;
    k.quoteVolume = 1.0;
    k.tradeCount = 1;
    k.isClosed = isClosed;
    out.push_back(k);
}

std::vector<Kline> makeBaseline(std::size_t count) {
    std::vector<Kline> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        pushCandle(out, 100.0, 101.0, 99.0, true);
    }
    return out;
}

std::vector<Kline> makeLongPattern(double prevClose, double close) {
    auto out = makeBaseline(84);
    pushCandle(out, 105.0, 106.0, 104.0, true);
    pushCandle(out, 110.0, 111.0, 109.0, true);
    pushCandle(out, 103.0, 104.0, 102.0, true);
    pushCandle(out, 97.0, 98.0, 96.0, true);
    pushCandle(out, 104.0, 105.0, 103.0, true);
    pushCandle(out, 112.0, 113.0, 111.0, true);
    pushCandle(out, 104.0, 105.0, 103.0, true);
    pushCandle(out, 99.0, 100.0, 98.0, true);
    pushCandle(out, 106.0, 107.0, 105.0, true);
    pushCandle(out, prevClose, prevClose + 1.0, prevClose - 1.0, true);
    pushCandle(out, close, close + 1.0, close - 1.0, true);
    return out;
}

std::vector<Kline> makeShortPattern(double prevClose, double close) {
    auto out = makeBaseline(84);
    pushCandle(out, 95.0, 96.0, 94.0, true);
    pushCandle(out, 90.0, 91.0, 89.0, true);
    pushCandle(out, 97.0, 98.0, 96.0, true);
    pushCandle(out, 103.0, 104.0, 102.0, true);
    pushCandle(out, 96.0, 97.0, 95.0, true);
    pushCandle(out, 88.0, 89.0, 87.0, true);
    pushCandle(out, 95.0, 96.0, 94.0, true);
    pushCandle(out, 101.0, 102.0, 100.0, true);
    pushCandle(out, 94.0, 95.0, 93.0, true);
    pushCandle(out, prevClose, prevClose + 1.0, prevClose - 1.0, true);
    pushCandle(out, close, close + 1.0, close - 1.0, true);
    return out;
}

std::vector<Kline> makeLongPatternWithExtraCandidates() {
    auto out = makeBaseline(84);

    // First high pivot: multiple candidate highs before reversal; strongest must be kept (112).
    pushCandle(out, 105.0, 106.0, 104.0, true);
    pushCandle(out, 110.0, 111.0, 109.0, true);
    pushCandle(out, 111.0, 112.0, 110.0, true);
    pushCandle(out, 104.0, 108.0, 103.0, true);

    // First low pivot: multiple candidate lows before reversal; strongest must be kept (96).
    pushCandle(out, 98.0, 104.0, 97.0, true);
    pushCandle(out, 97.0, 103.0, 96.0, true);
    pushCandle(out, 105.0, 106.0, 104.0, true);

    // Second high pivot with repeated high candidates; strongest must be kept (113).
    pushCandle(out, 108.0, 109.0, 107.0, true);
    pushCandle(out, 112.0, 113.0, 111.0, true);
    pushCandle(out, 111.0, 112.0, 110.0, true);
    pushCandle(out, 103.0, 108.0, 102.0, true);

    // Second low pivot with repeated low candidates; strongest must be kept (98).
    pushCandle(out, 100.0, 104.0, 99.0, true);
    pushCandle(out, 99.0, 103.0, 98.0, true);
    pushCandle(out, 105.0, 106.0, 104.0, true);

    // Crossing breakout above SH2 (=113): prevClose == 113, close == 114.
    pushCandle(out, 113.0, 114.0, 112.0, true);
    pushCandle(out, 114.0, 115.0, 113.0, true);

    return out;
}

} // namespace

TEST(DowTheoryPluginTest, EmitsLongSignalOnCrossingBreakout) {
    auto plugin = loadDowTheoryPlugin();
    auto strategy = createDowTheoryStrategy(plugin);

    const auto signal = strategy->evaluate("BTCUSDT", "30m", makeLongPattern(111.0, 114.0));
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::Long);
    EXPECT_GT(signal.confidence, 0.0);
    EXPECT_GT(signal.atr, 0.0);
    EXPECT_NE(signal.reason.find("Dow bull"), std::string::npos);
}

TEST(DowTheoryPluginTest, EmitsShortSignalOnCrossingBreakout) {
    auto plugin = loadDowTheoryPlugin();
    auto strategy = createDowTheoryStrategy(plugin);

    const auto signal = strategy->evaluate("BTCUSDT", "30m", makeShortPattern(88.0, 86.0));
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::Short);
    EXPECT_GT(signal.confidence, 0.0);
    EXPECT_GT(signal.atr, 0.0);
    EXPECT_NE(signal.reason.find("Dow bear"), std::string::npos);
}

TEST(DowTheoryPluginTest, IgnoresFormingCandleBreakout) {
    auto plugin = loadDowTheoryPlugin();
    auto strategy = createDowTheoryStrategy(plugin);

    auto klines = makeLongPattern(111.0, 111.0);
    pushCandle(klines, 114.0, 115.0, 113.0, false);

    const auto signal = strategy->evaluate("BTCUSDT", "30m", klines);
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::None);
}

TEST(DowTheoryPluginTest, DoesNotRepeatSameBreakoutWithoutCrossing) {
    auto plugin = loadDowTheoryPlugin();
    auto strategy = createDowTheoryStrategy(plugin);

    const auto signal = strategy->evaluate("BTCUSDT", "30m", makeLongPattern(114.0, 115.0));
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::None);
}

TEST(DowTheoryPluginTest, StrictEqualityDoesNotTriggerBreakout) {
    auto plugin = loadDowTheoryPlugin();
    auto strategy = createDowTheoryStrategy(plugin);

    const auto signal = strategy->evaluate("BTCUSDT", "30m", makeLongPattern(111.0, 113.0));
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::None);
}

TEST(DowTheoryPluginTest, ReturnsNoneWhenInsufficientClosedCandles) {
    auto plugin = loadDowTheoryPlugin();
    auto strategy = createDowTheoryStrategy(plugin);

    const auto signal = strategy->evaluate("BTCUSDT", "30m", makeBaseline(79));
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::None);
}

TEST(DowTheoryPluginTest, ReturnsNoneForUnconfiguredInterval) {
    auto plugin = loadDowTheoryPlugin();
    auto strategy = createDowTheoryStrategy(plugin);

    const auto signal = strategy->evaluate("BTCUSDT", "1h", makeLongPattern(111.0, 114.0));
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::None);
}

TEST(DowTheoryPluginTest, SameTypePivotNormalizationKeepsStrongerExtremes) {
    auto plugin = loadDowTheoryPlugin();
    auto strategy = createDowTheoryStrategy(plugin);

    const auto signal = strategy->evaluate("BTCUSDT", "30m", makeLongPatternWithExtraCandidates());
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::Long);
    EXPECT_GT(signal.confidence, 0.0);
    EXPECT_GT(signal.atr, 0.0);
    EXPECT_NE(signal.reason.find("SH1=112"), std::string::npos);
    EXPECT_NE(signal.reason.find("SH2=113"), std::string::npos);
    EXPECT_NE(signal.reason.find("SL1=96"), std::string::npos);
    EXPECT_NE(signal.reason.find("SL2=98"), std::string::npos);
}
