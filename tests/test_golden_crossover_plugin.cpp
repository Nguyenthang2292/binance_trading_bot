#include <gtest/gtest.h>

#include "catalog/plugin_handle.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kPluginFilename = "strategy_golden_crossover.dll";

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
        const std::vector<std::filesystem::path> candidates{
            current / "bin" / "Debug" / "plugins" / kPluginFilename,
            current / "bin" / "Release" / "plugins" / kPluginFilename,
            current / "build" / "bin" / "Debug" / "plugins" / kPluginFilename,
            current / "build" / "bin" / "Release" / "plugins" / kPluginFilename,
            current / "build" / "windows-msvc-debug" / "bin" / "Debug" / "plugins" / kPluginFilename,
            current / "plugins" / kPluginFilename,
        };
        for (const auto& candidate : candidates) {
            if (std::filesystem::exists(candidate, ec) && !ec) {
                return candidate;
            }
            ec.clear();
        }

        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

std::filesystem::path goldenPluginPath() {
    if (const auto byCwd = findPluginPathFrom(std::filesystem::current_path()); !byCwd.empty()) {
        return byCwd;
    }
    if (const auto byFile = findPluginPathFrom(std::filesystem::path(__FILE__).parent_path()); !byFile.empty()) {
        return byFile;
    }
    throw std::runtime_error("unable to locate strategy_golden_crossover.dll");
}

catalog::PluginHandle loadGoldenPlugin() {
    auto loaded = catalog::PluginHandle::load(goldenPluginPath());
    if (!loaded) {
        throw std::runtime_error(loaded.error());
    }
    return std::move(*loaded);
}

using StrategyPtr = std::unique_ptr<strategy::IStrategy, catalog::PluginHandle::DestroyFn>;

StrategyPtr createGoldenStrategy(catalog::PluginHandle& plugin, const std::string& config = "{}") {
    strategy::IStrategy* raw = plugin.create(config.c_str());
    if (!raw) {
        throw std::runtime_error("createStrategy returned nullptr");
    }
    return StrategyPtr(raw, plugin.destroyFunction());
}

void setKline(Kline& kline, size_t index, double close, bool isClosed = true) {
    kline.openTime = static_cast<int64_t>(1000 + index);
    kline.closeTime = static_cast<int64_t>(1999 + index);
    kline.open = close;
    kline.high = close + 2.0;
    kline.low = close - 2.0;
    kline.close = close;
    kline.volume = 1.0;
    kline.isClosed = isClosed;
}

std::vector<Kline> makeGoldenKlines(bool longSignal) {
    std::vector<Kline> klines(200);
    for (size_t i = 0; i < 150; ++i) {
        setKline(klines[i], i, 100.0);
    }
    for (size_t i = 150; i < klines.size(); ++i) {
        setKline(klines[i], i, longSignal ? 120.0 : 80.0);
    }
    return klines;
}

} // namespace

TEST(GoldenCrossoverPluginTest, LoadsDefaultConfig) {
    auto plugin = loadGoldenPlugin();
    auto strategy = createGoldenStrategy(plugin);

    EXPECT_EQ(strategy->config().name, "Golden 50/200 Moving Average Crossover (Crypto MTF State Variant)");
    EXPECT_EQ(strategy->config().type, "golden_crossover");
    EXPECT_EQ(strategy->config().intervals, (std::vector<std::string>{"4h", "1h", "30m"}));
    EXPECT_DOUBLE_EQ(strategy->config().takeProfitPercent, 20.0);
}

TEST(GoldenCrossoverPluginTest, EmitsLongAndShortFromClosedCandles) {
    auto plugin = loadGoldenPlugin();
    auto strategy = createGoldenStrategy(plugin);

    const auto longSignal = strategy->evaluate("BTCUSDT", "4h", makeGoldenKlines(true));
    EXPECT_EQ(longSignal.direction, strategy::Signal::Direction::Long);
    EXPECT_GT(longSignal.confidence, 0.5);
    EXPECT_GT(longSignal.atr, 0.0);
    EXPECT_NE(longSignal.reason.find("Golden Cross"), std::string::npos);

    const auto shortSignal = strategy->evaluate("BTCUSDT", "30m", makeGoldenKlines(false));
    EXPECT_EQ(shortSignal.direction, strategy::Signal::Direction::Short);
    EXPECT_GT(shortSignal.confidence, 0.5);
    EXPECT_GT(shortSignal.atr, 0.0);
    EXPECT_NE(shortSignal.reason.find("Death Cross"), std::string::npos);
}

TEST(GoldenCrossoverPluginTest, FormingCandleDoesNotAffectSignal) {
    auto plugin = loadGoldenPlugin();
    auto strategy = createGoldenStrategy(plugin);

    auto klines = makeGoldenKlines(true);
    Kline forming;
    setKline(forming, klines.size(), 1.0, false);
    klines.push_back(forming);

    const auto signal = strategy->evaluate("BTCUSDT", "1h", klines);

    EXPECT_EQ(signal.direction, strategy::Signal::Direction::Long);
    EXPECT_GT(signal.atr, 0.0);
}

TEST(GoldenCrossoverPluginTest, RejectsInvalidPeriodConfig) {
    auto plugin = loadGoldenPlugin();

    strategy::IStrategy* raw = plugin.create(R"json({"params":{"ma_short":200,"ma_long":50}})json");

    EXPECT_EQ(raw, nullptr);
}
