#include <gtest/gtest.h>

#include "catalog/plugin_handle.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kPluginFilename = "strategy_monthly_close_model.dll";

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

std::filesystem::path monthlyPluginPath() {
    if (const auto byCwd = findPluginPathFrom(std::filesystem::current_path()); !byCwd.empty()) {
        return byCwd;
    }
    if (const auto byFile = findPluginPathFrom(std::filesystem::path(__FILE__).parent_path()); !byFile.empty()) {
        return byFile;
    }
    throw std::runtime_error("unable to locate strategy_monthly_close_model.dll");
}

catalog::PluginHandle loadMonthlyPlugin() {
    auto loaded = catalog::PluginHandle::load(monthlyPluginPath());
    if (!loaded) {
        throw std::runtime_error(loaded.error());
    }
    return std::move(*loaded);
}

using StrategyPtr = std::unique_ptr<strategy::IStrategy, catalog::PluginHandle::DestroyFn>;

StrategyPtr createMonthlyStrategy(catalog::PluginHandle& plugin, const std::string& config = "{}") {
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

std::vector<Kline> makeMonthlyKlines(double firstClose, double lastClose, size_t count = 31) {
    std::vector<Kline> klines(count);
    for (size_t i = 0; i < klines.size(); ++i) {
        setKline(klines[i], i, firstClose);
    }
    klines.back().close = lastClose;
    klines.back().open = lastClose;
    klines.back().high = lastClose + 2.0;
    klines.back().low = lastClose - 2.0;
    return klines;
}

} // namespace

TEST(MonthlyCloseModelPluginTest, LoadsDefaultConfig) {
    auto plugin = loadMonthlyPlugin();
    auto strategy = createMonthlyStrategy(plugin);

    EXPECT_EQ(strategy->config().name, "Monthly Close Model (Adapted MTF)");
    EXPECT_EQ(strategy->config().type, "monthly_close_model");
    EXPECT_EQ(strategy->config().intervals, (std::vector<std::string>{"4h", "1h", "30m", "1d"}));
    EXPECT_DOUBLE_EQ(strategy->config().minConfidence, 0.001);
    EXPECT_DOUBLE_EQ(strategy->config().takeProfitPercent, 20.0);
}

TEST(MonthlyCloseModelPluginTest, EmitsLongShortAndNoneFromClosedCandles) {
    auto plugin = loadMonthlyPlugin();
    auto strategy = createMonthlyStrategy(plugin);

    const auto longSignal = strategy->evaluate("BTCUSDT", "4h", makeMonthlyKlines(100.0, 101.0));
    EXPECT_EQ(longSignal.direction, strategy::Signal::Direction::Long);
    EXPECT_DOUBLE_EQ(longSignal.confidence, 0.01);
    EXPECT_GT(longSignal.atr, 0.0);
    EXPECT_NE(longSignal.reason.find("Period+30 close up"), std::string::npos);

    const auto shortSignal = strategy->evaluate("BTCUSDT", "30m", makeMonthlyKlines(100.0, 99.0));
    EXPECT_EQ(shortSignal.direction, strategy::Signal::Direction::Short);
    EXPECT_DOUBLE_EQ(shortSignal.confidence, 0.01);
    EXPECT_GT(shortSignal.atr, 0.0);
    EXPECT_NE(shortSignal.reason.find("Period+30 close dn"), std::string::npos);

    const auto noneSignal = strategy->evaluate("BTCUSDT", "1h", makeMonthlyKlines(100.0, 100.0));
    EXPECT_EQ(noneSignal.direction, strategy::Signal::Direction::None);
}

TEST(MonthlyCloseModelPluginTest, FormingCandleDoesNotAffectSignal) {
    auto plugin = loadMonthlyPlugin();
    auto strategy = createMonthlyStrategy(plugin);

    auto klines = makeMonthlyKlines(100.0, 101.0);
    Kline forming;
    setKline(forming, klines.size(), 1.0, false);
    klines.push_back(forming);

    const auto signal = strategy->evaluate("BTCUSDT", "4h", klines);

    EXPECT_EQ(signal.direction, strategy::Signal::Direction::Long);
    EXPECT_GT(signal.atr, 0.0);
}

TEST(MonthlyCloseModelPluginTest, RequiresOnlyPeriodPlusOneClosedCandles) {
    auto plugin = loadMonthlyPlugin();
    auto strategy = createMonthlyStrategy(plugin);

    const auto tooShort = strategy->evaluate("BTCUSDT", "4h", makeMonthlyKlines(100.0, 101.0, 30));
    EXPECT_EQ(tooShort.direction, strategy::Signal::Direction::None);

    const auto enough = strategy->evaluate("BTCUSDT", "4h", makeMonthlyKlines(100.0, 101.0, 31));
    EXPECT_EQ(enough.direction, strategy::Signal::Direction::Long);
}

TEST(MonthlyCloseModelPluginTest, RejectsInvalidConfig) {
    auto plugin = loadMonthlyPlugin();

    strategy::IStrategy* badPeriod = plugin.create(R"json({"params":{"period":0}})json");
    EXPECT_EQ(badPeriod, nullptr);

    strategy::IStrategy* badHold = plugin.create(
        R"json({"max_hold_duration_by_interval_seconds":{"4h":0}})json");
    EXPECT_EQ(badHold, nullptr);
}
