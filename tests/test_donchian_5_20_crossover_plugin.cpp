#include <gtest/gtest.h>

#include "catalog/plugin_handle.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* kPluginFilename = "strategy_donchian_5_20_crossover.dll";

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

std::filesystem::path donchianPluginPath() {
    if (const auto byCwd = findPluginPathFrom(std::filesystem::current_path()); !byCwd.empty()) {
        return byCwd;
    }
    if (const auto byFile = findPluginPathFrom(std::filesystem::path(__FILE__).parent_path()); !byFile.empty()) {
        return byFile;
    }
    throw std::runtime_error("unable to locate strategy_donchian_5_20_crossover.dll");
}

catalog::PluginHandle loadDonchianPlugin() {
    auto loaded = catalog::PluginHandle::load(donchianPluginPath());
    if (!loaded) {
        throw std::runtime_error(loaded.error());
    }
    return std::move(*loaded);
}

using StrategyPtr = std::unique_ptr<strategy::IStrategy, catalog::PluginHandle::DestroyFn>;

StrategyPtr createDonchianStrategy(catalog::PluginHandle& plugin, const std::string& config = "{}") {
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

std::vector<Kline> makeDonchianKlines(bool longSignal) {
    std::vector<Kline> klines(20);
    for (size_t i = 0; i < 15; ++i) {
        setKline(klines[i], i, 100.0);
    }
    for (size_t i = 15; i < 20; ++i) {
        setKline(klines[i], i, longSignal ? 120.0 : 80.0);
    }
    return klines;
}

} // namespace

TEST(Donchian520PluginTest, LoadsDefaultVariantConfig) {
    auto plugin = loadDonchianPlugin();
    auto strategy = createDonchianStrategy(plugin);

    EXPECT_EQ(strategy->config().name, "Donchian 5 and 20-Day Crossover (Crypto MTF State Variant)");
    EXPECT_EQ(strategy->config().type, "donchian_5_20_crossover");
    EXPECT_EQ(strategy->config().intervals, (std::vector<std::string>{"1d", "4h", "1h", "30m"}));
    EXPECT_DOUBLE_EQ(strategy->config().minConfidence, 0.5);
    EXPECT_DOUBLE_EQ(strategy->config().takeProfitPercent, 20.0);
}

TEST(Donchian520PluginTest, EmitsLongAndShortFromClosedCandles) {
    auto plugin = loadDonchianPlugin();
    auto strategy = createDonchianStrategy(plugin);

    const auto longSignal = strategy->evaluate("BTCUSDT", "1d", makeDonchianKlines(true));
    EXPECT_EQ(longSignal.direction, strategy::Signal::Direction::Long);
    EXPECT_DOUBLE_EQ(longSignal.confidence, 1.0);
    EXPECT_GT(longSignal.atr, 0.0);
    EXPECT_NE(longSignal.reason.find("SMA5="), std::string::npos);
    EXPECT_NE(longSignal.reason.find("> SMA20="), std::string::npos);

    const auto shortSignal = strategy->evaluate("BTCUSDT", "30m", makeDonchianKlines(false));
    EXPECT_EQ(shortSignal.direction, strategy::Signal::Direction::Short);
    EXPECT_DOUBLE_EQ(shortSignal.confidence, 1.0);
    EXPECT_GT(shortSignal.atr, 0.0);
    EXPECT_NE(shortSignal.reason.find("SMA5="), std::string::npos);
    EXPECT_NE(shortSignal.reason.find("< SMA20="), std::string::npos);
}

TEST(Donchian520PluginTest, FormingCandleDoesNotAffectSignal) {
    auto plugin = loadDonchianPlugin();
    auto strategy = createDonchianStrategy(plugin);

    auto klines = makeDonchianKlines(true);
    Kline forming;
    setKline(forming, klines.size(), 0.0, false);
    klines.push_back(forming);

    const auto signal = strategy->evaluate("BTCUSDT", "1d", klines);

    EXPECT_EQ(signal.direction, strategy::Signal::Direction::Long);
    EXPECT_GT(signal.atr, 0.0);
}

TEST(Donchian520PluginTest, ReturnsNoneWhenAllKlinesAreForming) {
    auto plugin = loadDonchianPlugin();
    auto strategy = createDonchianStrategy(plugin);

    auto klines = makeDonchianKlines(true);
    for (auto& k : klines) {
        k.isClosed = false;
    }

    const auto signal = strategy->evaluate("BTCUSDT", "1h", klines);

    EXPECT_EQ(signal.direction, strategy::Signal::Direction::None);
}

TEST(Donchian520PluginTest, RejectsInvalidPeriodConfig) {
    auto plugin = loadDonchianPlugin();

    strategy::IStrategy* raw = plugin.create(R"json({"params":{"short_period":20,"long_period":5}})json");

    EXPECT_EQ(raw, nullptr);
}
