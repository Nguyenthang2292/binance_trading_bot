#include <gtest/gtest.h>

#include "catalog/plugin_handle.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr const char* kPluginFilename = "strategy_ricardo_rules.dll";

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

std::filesystem::path ricardoRulesPluginPath() {
    if (const auto byCwd = findPluginPathFrom(std::filesystem::current_path()); !byCwd.empty()) {
        return byCwd;
    }
    if (const auto byFile = findPluginPathFrom(std::filesystem::path(__FILE__).parent_path()); !byFile.empty()) {
        return byFile;
    }
    for (const auto& config : {"Debug", "Release", "RelWithDebInfo"}) {
        const auto candidate =
            std::filesystem::path(BOT_SOURCE_DIR) / "build" / "bin" / config / "plugins" / kPluginFilename;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }
    }
    throw std::runtime_error("unable to locate strategy_ricardo_rules.dll");
}

catalog::PluginHandle loadRicardoRulesPlugin() {
    auto loaded = catalog::PluginHandle::load(ricardoRulesPluginPath());
    if (!loaded) {
        throw std::runtime_error(loaded.error());
    }
    return std::move(*loaded);
}

using StrategyPtr = std::unique_ptr<strategy::IStrategy, catalog::PluginHandle::DestroyFn>;

StrategyPtr createRicardoRulesStrategy(catalog::PluginHandle& plugin) {
    const std::string cfg = R"json(
{
  "name": "Ricardo Rules",
  "type": "ricardo_rules",
  "intervals": ["30m"],
  "atr_period": 14,
  "min_confidence": 0.0,
  "takeProfitPercent": 0.0,
  "tp_multiplier": 0.0,
  "params": {
    "exit_policy": "swing_trailing",
    "fixed_take_profit": false,
    "swing_lookback": 3
  }
}
)json";
    strategy::IStrategy* raw = plugin.create(cfg.c_str());
    if (!raw) {
        throw std::runtime_error("createStrategy returned nullptr");
    }
    return StrategyPtr(raw, plugin.destroyFunction());
}

std::vector<Kline> makeRicardoKlines(double setupHigh, double setupLow, double evalHigh, double evalLow, double evalClose) {
    std::vector<Kline> klines(16);
    for (size_t i = 0; i < klines.size(); ++i) {
        auto& k = klines[i];
        k.openTime = static_cast<int64_t>(1000 + i);
        k.high = 101.0;
        k.low = 99.0;
        k.close = 100.0;
        k.isClosed = true;
    }

    auto& setup = klines[13];
    setup.high = setupHigh;
    setup.low = setupLow;
    setup.close = (setupHigh + setupLow) / 2.0;

    auto& eval = klines[14];
    eval.high = evalHigh;
    eval.low = evalLow;
    eval.close = evalClose;

    klines[15].high = 1000.0;
    klines[15].low = 1.0;
    klines[15].close = 500.0;
    klines[15].isClosed = false;
    return klines;
}

} // namespace

TEST(RicardoRulesPluginTest, EmitsLongBreakoutWithExecutionPlanFields) {
    auto plugin = loadRicardoRulesPlugin();
    auto strategy = createRicardoRulesStrategy(plugin);

    EXPECT_DOUBLE_EQ(strategy->config().takeProfitPercent, 0.0);
    EXPECT_DOUBLE_EQ(strategy->config().tpMultiplier, 0.0);
    EXPECT_DOUBLE_EQ(strategy->config().minConfidence, 0.0);

    const auto signal = strategy->evaluate("BTCUSDT", "30m", makeRicardoKlines(105.0, 98.0, 106.5, 99.0, 106.0));

    EXPECT_EQ(signal.direction, strategy::Signal::Direction::Long);
    EXPECT_GT(signal.confidence, 0.0);
    EXPECT_GT(signal.atr, 0.0);
    EXPECT_DOUBLE_EQ(signal.initialStopPrice, 98.0);
    EXPECT_TRUE(signal.disableFixedTakeProfit);
    EXPECT_EQ(signal.exitPolicy, strategy::Signal::ExitPolicy::SwingTrailing);
    EXPECT_EQ(signal.swingLookback, 3);
    EXPECT_NE(signal.reason.find("30m Ricardo breakout long"), std::string::npos);
    EXPECT_NE(signal.reason.find("initial_stop=98"), std::string::npos);
}

TEST(RicardoRulesPluginTest, EmitsShortBreakoutWithExecutionPlanFields) {
    auto plugin = loadRicardoRulesPlugin();
    auto strategy = createRicardoRulesStrategy(plugin);

    const auto signal = strategy->evaluate("BTCUSDT", "30m", makeRicardoKlines(105.0, 98.0, 104.0, 96.0, 97.0));

    EXPECT_EQ(signal.direction, strategy::Signal::Direction::Short);
    EXPECT_GT(signal.confidence, 0.0);
    EXPECT_GT(signal.atr, 0.0);
    EXPECT_DOUBLE_EQ(signal.initialStopPrice, 105.0);
    EXPECT_TRUE(signal.disableFixedTakeProfit);
    EXPECT_EQ(signal.exitPolicy, strategy::Signal::ExitPolicy::SwingTrailing);
    EXPECT_EQ(signal.swingLookback, 3);
    EXPECT_NE(signal.reason.find("30m Ricardo breakout short"), std::string::npos);
    EXPECT_NE(signal.reason.find("initial_stop=105"), std::string::npos);
}

TEST(RicardoRulesPluginTest, RequiresStrictBreakoutAndConfiguredInterval) {
    auto plugin = loadRicardoRulesPlugin();
    auto strategy = createRicardoRulesStrategy(plugin);

    const auto equalHigh = strategy->evaluate("BTCUSDT", "30m", makeRicardoKlines(105.0, 98.0, 106.0, 99.0, 105.0));
    EXPECT_EQ(equalHigh.direction, strategy::Signal::Direction::None);

    const auto unconfiguredInterval =
        strategy->evaluate("BTCUSDT", "1h", makeRicardoKlines(105.0, 98.0, 106.5, 99.0, 106.0));
    EXPECT_EQ(unconfiguredInterval.direction, strategy::Signal::Direction::None);
}
