#include <gtest/gtest.h>

#include "catalog/plugin_handle.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* kPluginFilename = "strategy_gartley_day_crossover.dll";

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

std::filesystem::path gartleyPluginPath() {
    if (const auto byCwd = findPluginPathFrom(std::filesystem::current_path()); !byCwd.empty()) {
        return byCwd;
    }
    if (const auto byFile = findPluginPathFrom(std::filesystem::path(__FILE__).parent_path()); !byFile.empty()) {
        return byFile;
    }
    throw std::runtime_error("unable to locate strategy_gartley_day_crossover.dll");
}

catalog::PluginHandle loadGartleyPlugin() {
    auto loaded = catalog::PluginHandle::load(gartleyPluginPath());
    if (!loaded) {
        throw std::runtime_error(loaded.error());
    }
    return std::move(*loaded);
}

using StrategyPtr = std::unique_ptr<strategy::IStrategy, catalog::PluginHandle::DestroyFn>;

StrategyPtr createGartleyStrategy(catalog::PluginHandle& plugin, const std::string& config = "{}") {
    strategy::IStrategy* raw = plugin.create(config.c_str());
    if (!raw) {
        throw std::runtime_error("createStrategy returned nullptr");
    }
    return StrategyPtr(raw, plugin.destroyFunction());
}

enum class GartleyFixture {
    Long,
    Short,
    None,
    EqualUpper,
};

void setKline(Kline& kline, size_t index, double high, double low, double close, bool isClosed = true) {
    kline.openTime = static_cast<int64_t>(1000 + index);
    kline.closeTime = static_cast<int64_t>(1999 + index);
    kline.open = close;
    kline.high = high;
    kline.low = low;
    kline.close = close;
    kline.volume = 1.0;
    kline.isClosed = isClosed;
}

std::vector<Kline> makeGartleyKlines(GartleyFixture fixture) {
    std::vector<Kline> klines(16);
    for (size_t i = 0; i < klines.size(); ++i) {
        setKline(klines[i], i, 100.0, 90.0, 95.0, i + 1 < klines.size());
    }

    switch (fixture) {
        case GartleyFixture::Long:
            setKline(klines[13], 13, 112.0, 108.0, 110.0);
            setKline(klines[14], 14, 112.0, 108.0, 110.0);
            break;
        case GartleyFixture::Short:
            setKline(klines[13], 13, 82.0, 78.0, 80.0);
            setKline(klines[14], 14, 82.0, 78.0, 80.0);
            break;
        case GartleyFixture::EqualUpper:
            setKline(klines[13], 13, 105.0, 100.0, 102.5);
            setKline(klines[14], 14, 105.0, 100.0, 102.5);
            break;
        case GartleyFixture::None:
            break;
    }

    klines.back().isClosed = false;
    return klines;
}

} // namespace

TEST(GartleyDayCrossoverPluginTest, LoadsDefaultConfig) {
    auto plugin = loadGartleyPlugin();
    auto strategy = createGartleyStrategy(plugin);

    EXPECT_EQ(strategy->config().name, "Gartley 3&6 Candle Crossover");
    EXPECT_EQ(strategy->config().type, "gartley_day_crossover");
    EXPECT_EQ(strategy->config().intervals, (std::vector<std::string>{"1d", "4h", "1h", "30m"}));
    EXPECT_DOUBLE_EQ(strategy->config().minNotional, 1.0);
}

TEST(GartleyDayCrossoverPluginTest, EmitsLongShortAndNone) {
    auto plugin = loadGartleyPlugin();
    auto strategy = createGartleyStrategy(plugin);

    const auto longSignal = strategy->evaluate("BTCUSDT", "30m", makeGartleyKlines(GartleyFixture::Long));
    EXPECT_EQ(longSignal.direction, strategy::Signal::Direction::Long);
    EXPECT_DOUBLE_EQ(longSignal.confidence, 1.0);
    EXPECT_GT(longSignal.atr, 0.0);
    EXPECT_NE(longSignal.reason.find("30m Gartley long"), std::string::npos);

    const auto shortSignal = strategy->evaluate("BTCUSDT", "1h", makeGartleyKlines(GartleyFixture::Short));
    EXPECT_EQ(shortSignal.direction, strategy::Signal::Direction::Short);
    EXPECT_DOUBLE_EQ(shortSignal.confidence, 1.0);
    EXPECT_GT(shortSignal.atr, 0.0);
    EXPECT_NE(shortSignal.reason.find("1h Gartley short"), std::string::npos);

    const auto noneSignal = strategy->evaluate("BTCUSDT", "4h", makeGartleyKlines(GartleyFixture::None));
    EXPECT_EQ(noneSignal.direction, strategy::Signal::Direction::None);
}

TEST(GartleyDayCrossoverPluginTest, EqualityDoesNotTrigger) {
    auto plugin = loadGartleyPlugin();
    auto strategy = createGartleyStrategy(plugin);

    const auto signal = strategy->evaluate("BTCUSDT", "1d", makeGartleyKlines(GartleyFixture::EqualUpper));

    EXPECT_EQ(signal.direction, strategy::Signal::Direction::None);
}

TEST(GartleyDayCrossoverPluginTest, RequiresEnoughClosedCandlesForAtr) {
    auto plugin = loadGartleyPlugin();
    auto strategy = createGartleyStrategy(plugin);

    auto klines = makeGartleyKlines(GartleyFixture::Long);
    klines.pop_back();

    const auto signal = strategy->evaluate("BTCUSDT", "30m", klines);

    EXPECT_EQ(signal.direction, strategy::Signal::Direction::None);
}

TEST(GartleyDayCrossoverPluginTest, FormingCandleDoesNotAffectAtr) {
    auto plugin = loadGartleyPlugin();
    auto strategy = createGartleyStrategy(plugin);

    auto normal = makeGartleyKlines(GartleyFixture::Long);
    auto wildForming = normal;
    setKline(wildForming.back(), 15, 10000.0, 1.0, 5000.0, false);

    const auto normalSignal = strategy->evaluate("BTCUSDT", "30m", normal);
    const auto wildSignal = strategy->evaluate("BTCUSDT", "30m", wildForming);

    EXPECT_EQ(normalSignal.direction, strategy::Signal::Direction::Long);
    EXPECT_EQ(wildSignal.direction, strategy::Signal::Direction::Long);
    EXPECT_DOUBLE_EQ(normalSignal.atr, wildSignal.atr);
}
