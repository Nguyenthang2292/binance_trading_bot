#include <gtest/gtest.h>

#include "catalog/plugin_handle.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* kPluginFilename = "strategy_trend_breakout.dll";

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

        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

std::filesystem::path trendBreakoutPluginPath() {
    if (const auto byCwd = findPluginPathFrom(std::filesystem::current_path()); !byCwd.empty()) {
        return byCwd;
    }
    if (const auto byFile = findPluginPathFrom(std::filesystem::path(__FILE__).parent_path()); !byFile.empty()) {
        return byFile;
    }
    throw std::runtime_error("unable to locate strategy_trend_breakout.dll");
}

catalog::PluginHandle loadTrendBreakoutPlugin() {
    auto loaded = catalog::PluginHandle::load(trendBreakoutPluginPath());
    if (!loaded) {
        throw std::runtime_error(loaded.error());
    }
    return std::move(*loaded);
}

using StrategyPtr = std::unique_ptr<strategy::IStrategy, catalog::PluginHandle::DestroyFn>;

StrategyPtr createTrendBreakoutStrategy(catalog::PluginHandle& plugin) {
    const std::string cfg = R"json(
{
  "name": "Trend Breakout Trader",
  "type": "trend_breakout",
  "intervals": ["30m", "1h", "4h"],
  "atr_period": 14,
  "min_confidence": 0.5,
  "params": {
    "breakout_period": 20,
    "trailing_enabled": true,
    "trailing_candles": 42,
    "trailing_check_interval_seconds": 300
  }
}
)json";
    strategy::IStrategy* raw = plugin.create(cfg.c_str());
    if (!raw) {
        throw std::runtime_error("createStrategy returned nullptr");
    }
    return StrategyPtr(raw, plugin.destroyFunction());
}

std::vector<Kline> makeBreakoutKlines(double evalClose) {
    std::vector<Kline> klines(22);
    for (size_t i = 0; i < klines.size(); ++i) {
        auto& k = klines[i];
        k.openTime = static_cast<int64_t>(1000 + i);
        k.high = 100.0;
        k.low = 90.0;
        k.close = 95.0;
        k.isClosed = true;
    }

    auto& eval = klines[20];
    eval.close = evalClose;
    eval.high = std::max(100.0, evalClose + 1.0);
    eval.low = std::min(90.0, evalClose - 1.0);

    // Last candle can be forming; strategy evaluates klines[size - 2].
    klines[21].isClosed = false;
    return klines;
}

void expectLongSignal(
    const strategy::Signal& signal,
    std::string_view interval,
    std::string_view expectedReasonPart) {
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::Long);
    EXPECT_GT(signal.atr, 0.0);
    EXPECT_NE(signal.reason.find(std::string(interval) + " Donchian breakout long"), std::string::npos);
    EXPECT_NE(signal.reason.find(std::string(expectedReasonPart)), std::string::npos);
}

void expectShortSignal(
    const strategy::Signal& signal,
    std::string_view interval,
    std::string_view expectedReasonPart) {
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::Short);
    EXPECT_GT(signal.atr, 0.0);
    EXPECT_NE(signal.reason.find(std::string(interval) + " Donchian breakout short"), std::string::npos);
    EXPECT_NE(signal.reason.find(std::string(expectedReasonPart)), std::string::npos);
}

} // namespace

TEST(TrendBreakoutPluginTest, EmitsLongAndShortFor30m) {
    auto plugin = loadTrendBreakoutPlugin();
    auto strategy = createTrendBreakoutStrategy(plugin);
    EXPECT_DOUBLE_EQ(strategy->config().takeProfitPercent, 20.0);

    const auto longSignal = strategy->evaluate("BTCUSDT", "30m", makeBreakoutKlines(101.0));
    expectLongSignal(longSignal, "30m", "close=101 > high20=100");

    const auto shortSignal = strategy->evaluate("BTCUSDT", "30m", makeBreakoutKlines(89.0));
    expectShortSignal(shortSignal, "30m", "close=89 < low20=90");
}

TEST(TrendBreakoutPluginTest, EmitsLongAndShortFor1h) {
    auto plugin = loadTrendBreakoutPlugin();
    auto strategy = createTrendBreakoutStrategy(plugin);

    const auto longSignal = strategy->evaluate("BTCUSDT", "1h", makeBreakoutKlines(101.0));
    expectLongSignal(longSignal, "1h", "close=101 > high20=100");

    const auto shortSignal = strategy->evaluate("BTCUSDT", "1h", makeBreakoutKlines(89.0));
    expectShortSignal(shortSignal, "1h", "close=89 < low20=90");
}

TEST(TrendBreakoutPluginTest, FourHourSignalStillWorks) {
    auto plugin = loadTrendBreakoutPlugin();
    auto strategy = createTrendBreakoutStrategy(plugin);

    const auto signal = strategy->evaluate("BTCUSDT", "4h", makeBreakoutKlines(101.0));
    expectLongSignal(signal, "4h", "close=101 > high20=100");
}

TEST(TrendBreakoutPluginTest, ReturnsNoSignalForUnconfiguredInterval) {
    auto plugin = loadTrendBreakoutPlugin();
    auto strategy = createTrendBreakoutStrategy(plugin);

    const auto signal = strategy->evaluate("BTCUSDT", "15m", makeBreakoutKlines(101.0));
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::None);
    EXPECT_TRUE(signal.reason.empty());
}
