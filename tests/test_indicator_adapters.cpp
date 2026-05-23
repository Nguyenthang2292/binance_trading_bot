// Adapter-vs-plugin parity tests.
//
// Critical defense against adapter drift: each whitelisted strategy's plugin
// formula must produce the same signal as the corresponding adapter when given
// matching params + klines.
//
// See docs/sdk/plugin-review-checklist.md for the contract.

#include "backtest/indicator_adapters.h"
#include "backtest/optimizable_strategy.h"
#include "catalog/plugin_handle.h"
#include "strategy/istrategy.h"
#include "strategy/strategy_config.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path findPlugin(const std::string& filename) {
    fs::path start = fs::current_path();
    while (!start.empty()) {
        const std::vector<fs::path> candidates{
            start / "bin"   / "Debug"   / "plugins" / filename,
            start / "bin"   / "Release" / "plugins" / filename,
            start / "build" / "bin" / "Debug"   / "plugins" / filename,
            start / "build" / "bin" / "Release" / "plugins" / filename,
            start / "build" / "windows-msvc-debug" / "bin" / "Debug" / "plugins" / filename,
            start / "plugins" / filename,
        };
        for (const auto& c : candidates) {
            std::error_code ec;
            if (fs::exists(c, ec) && !ec) return c;
        }
        const auto parent = start.parent_path();
        if (parent == start) break;
        start = parent;
    }
    // Fallback: relative to source file.
    fs::path src = fs::path(__FILE__).parent_path();
    while (!src.empty()) {
        const std::vector<fs::path> candidates{
            src / "build" / "bin" / "Debug"   / "plugins" / filename,
            src / "build" / "bin" / "Release" / "plugins" / filename,
            src / "plugins" / filename,
        };
        for (const auto& c : candidates) {
            std::error_code ec;
            if (fs::exists(c, ec) && !ec) return c;
        }
        const auto parent = src.parent_path();
        if (parent == src) break;
        src = parent;
    }
    return {};
}

catalog::PluginHandle loadPlugin(const std::string& filename) {
    auto path = findPlugin(filename);
    if (path.empty()) throw std::runtime_error("plugin not found: " + filename);
    auto loaded = catalog::PluginHandle::load(path);
    if (!loaded) throw std::runtime_error(loaded.error());
    return std::move(*loaded);
}

using StrategyPtr = std::unique_ptr<strategy::IStrategy, catalog::PluginHandle::DestroyFn>;

StrategyPtr createStrategy(catalog::PluginHandle& plugin, const std::string& json = "{}") {
    auto* raw = plugin.create(json.c_str());
    if (!raw) throw std::runtime_error("plugin create returned null");
    return StrategyPtr(raw, plugin.destroyFunction());
}

void setBar(Kline& k, size_t i, double close, bool closed = true) {
    k.openTime  = static_cast<int64_t>(1000 + i);
    k.closeTime = static_cast<int64_t>(1999 + i);
    k.open  = close;
    k.high  = close + 2.0;
    k.low   = close - 2.0;
    k.close = close;
    k.volume = 1.0;
    k.isClosed = closed;
}

std::vector<Kline> makeTrendKlines(size_t n, double startPrice, double endPrice) {
    std::vector<Kline> ks(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(std::max<size_t>(1, n - 1));
        setBar(ks[i], i, startPrice + t * (endPrice - startPrice));
    }
    return ks;
}

bool signalsEqual(const strategy::Signal& a, const strategy::Signal& b, double tol = 1e-9) {
    if (a.direction != b.direction) return false;
    if (std::abs(a.atr - b.atr) > tol) return false;
    if (std::abs(a.confidence - b.confidence) > tol) return false;
    return true;
}

}  // namespace

// --------------------------------- spec tests ---------------------------------

TEST(IndicatorAdaptersTest, GoldenSpecHasConstraintMaShortLessMaLong) {
    backtest::GoldenCrossoverAdapter a;
    auto s = a.spec(strategy::StrategyConfig{});
    ASSERT_EQ(s.constraints.size(), 1u);
    EXPECT_EQ(s.constraints[0].left,  "ma_short");
    EXPECT_EQ(s.constraints[0].right, "ma_long");
    EXPECT_EQ(s.constraints[0].kind, backtest::ParamConstraint::Kind::LessThan);
}

TEST(IndicatorAdaptersTest, DonchianSpecHasConstraintShortLessLong) {
    backtest::Donchian520CrossoverAdapter a;
    auto s = a.spec(strategy::StrategyConfig{});
    ASSERT_EQ(s.constraints.size(), 1u);
    EXPECT_EQ(s.constraints[0].left, "short_period");
}

TEST(IndicatorAdaptersTest, GartleyConfThresholdRangeIsRealistic) {
    // Regression: earlier 0.3..0.8 range was a unit error.
    backtest::GartleyDayCrossoverAdapter a;
    auto s = a.spec(strategy::StrategyConfig{});
    for (const auto& r : s.defaults) {
        if (r.name == "conf_threshold") {
            EXPECT_LE(r.max, 0.1) << "conf_threshold range too high";
            EXPECT_GE(r.min, 0.001) << "conf_threshold range too low";
        }
    }
}

// --------------------------- plugin parity tests -----------------------------
// These load the actual plugin DLL/SO and compare signal-for-signal with the
// adapter at the strategy's documented defaults. Any divergence in formula or
// confidence calculation will surface here.

TEST(IndicatorAdaptersTest, GoldenAdapterMatchesPluginOnTrendingKlines) {
    auto plugin = loadPlugin("strategy_golden_crossover.dll");
    auto strat  = createStrategy(plugin);

    // Match plugin's default min_confidence=0.5 from parseConfig().
    strategy::StrategyConfig base;
    base.atrPeriod = 14;
    base.minConfidence = 0.5;

    backtest::GoldenCrossoverAdapter adapter;
    auto spec = adapter.spec(base);

    const auto klinesLong  = makeTrendKlines(220, 100.0, 130.0);
    const auto klinesShort = makeTrendKlines(220, 130.0, 100.0);

    const auto pluginLong  = strat->evaluate("BTCUSDT", "4h", klinesLong);
    const auto adapterLong = adapter.evaluateWith("BTCUSDT", "4h", klinesLong,
                                                  spec.currentValues, base);
    EXPECT_TRUE(signalsEqual(pluginLong, adapterLong))
        << "Long: plugin atr=" << pluginLong.atr << " conf=" << pluginLong.confidence
        << " vs adapter atr=" << adapterLong.atr << " conf=" << adapterLong.confidence;

    const auto pluginShort  = strat->evaluate("BTCUSDT", "4h", klinesShort);
    const auto adapterShort = adapter.evaluateWith("BTCUSDT", "4h", klinesShort,
                                                   spec.currentValues, base);
    EXPECT_TRUE(signalsEqual(pluginShort, adapterShort));
}

TEST(IndicatorAdaptersTest, DonchianAdapterMatchesPluginOnTrendingKlines) {
    auto plugin = loadPlugin("strategy_donchian_5_20_crossover.dll");
    auto strat  = createStrategy(plugin);

    strategy::StrategyConfig base;
    base.atrPeriod = 14;
    base.minConfidence = 0.5;

    backtest::Donchian520CrossoverAdapter adapter;
    auto spec = adapter.spec(base);

    const auto klinesUp   = makeTrendKlines(50, 100.0, 130.0);
    const auto klinesDown = makeTrendKlines(50, 130.0, 100.0);

    const auto a = strat->evaluate("ETHUSDT", "1h", klinesUp);
    const auto b = adapter.evaluateWith("ETHUSDT", "1h", klinesUp, spec.currentValues, base);
    EXPECT_TRUE(signalsEqual(a, b));

    const auto c = strat->evaluate("ETHUSDT", "1h", klinesDown);
    const auto d = adapter.evaluateWith("ETHUSDT", "1h", klinesDown, spec.currentValues, base);
    EXPECT_TRUE(signalsEqual(c, d));
}

TEST(IndicatorAdaptersTest, GartleyAdapterMatchesPluginOnTrendingKlines) {
    auto plugin = loadPlugin("strategy_gartley_day_crossover.dll");
    auto strat  = createStrategy(plugin);

    strategy::StrategyConfig base;
    base.atrPeriod = 14;
    base.minConfidence = 0.5;

    backtest::GartleyDayCrossoverAdapter adapter;
    auto spec = adapter.spec(base);

    const auto klinesUp   = makeTrendKlines(40, 100.0, 130.0);
    const auto klinesDown = makeTrendKlines(40, 130.0, 100.0);

    const auto a = strat->evaluate("ETHUSDT", "1h", klinesUp);
    const auto b = adapter.evaluateWith("ETHUSDT", "1h", klinesUp, spec.currentValues, base);
    EXPECT_TRUE(signalsEqual(a, b))
        << "Long: plugin dir=" << static_cast<int>(a.direction)
        << " conf=" << a.confidence
        << " vs adapter dir=" << static_cast<int>(b.direction)
        << " conf=" << b.confidence;

    const auto c = strat->evaluate("ETHUSDT", "1h", klinesDown);
    const auto d = adapter.evaluateWith("ETHUSDT", "1h", klinesDown, spec.currentValues, base);
    EXPECT_TRUE(signalsEqual(c, d));
}
