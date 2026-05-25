// Indicator adapters — these reimplement the parameterized signal formulas of
// the corresponding plugins so the BacktestGate can sweep params without
// changing the plugin ABI.
//
// CRITICAL: any formula change in a plugin under plugins/src/<name>/ MUST be
// mirrored here. See docs/sdk/plugin-review-checklist.md and the parity tests
// in tests/test_indicator_adapters.cpp.

#include "backtest/indicator_adapters.h"
#include "strategy/indicators/atr.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace backtest {

namespace {

// Prefix-sum cache for SMAs over a stable Kline buffer (e.g., the
// BacktestEngine's growing window, which is pre-reserved and never reallocates
// within a runFold). Cumulatives are extended monotonically across calls,
// turning per-call SMA cost from O(period) into amortized O(1). Reset triggers:
// different data pointer, different first-bar openTime fingerprint (defends
// against allocator pointer reuse across runFolds), or cache larger than
// buffer (defensive sync). thread_local so concurrent workers don't share
// state.
struct PrefixSumCache {
    const Kline* dataPtr{nullptr};
    long long firstOpenTime{0};
    std::vector<double> close;  // close[i] = sum of klines[0..i-1].close, close[0]=0
    std::vector<double> high;
    std::vector<double> low;
};

thread_local PrefixSumCache g_prefixCache;

void ensurePrefixCache(const std::vector<Kline>& klines) {
    if (klines.empty()) {
        g_prefixCache = PrefixSumCache{};
        return;
    }
    const Kline* dataPtr = klines.data();
    const long long firstOpenTime = klines.front().openTime;
    const bool needReset = g_prefixCache.dataPtr != dataPtr
        || g_prefixCache.firstOpenTime != firstOpenTime
        || g_prefixCache.close.empty()
        || g_prefixCache.close.size() > klines.size() + 1;
    if (needReset) {
        g_prefixCache.dataPtr = dataPtr;
        g_prefixCache.firstOpenTime = firstOpenTime;
        g_prefixCache.close.assign(1, 0.0);
        g_prefixCache.high.assign(1, 0.0);
        g_prefixCache.low.assign(1, 0.0);
        g_prefixCache.close.reserve(klines.size() + 1);
        g_prefixCache.high.reserve(klines.size() + 1);
        g_prefixCache.low.reserve(klines.size() + 1);
    }
    while (g_prefixCache.close.size() <= klines.size()) {
        const std::size_t i = g_prefixCache.close.size() - 1;
        g_prefixCache.close.push_back(g_prefixCache.close.back() + klines[i].close);
        g_prefixCache.high.push_back(g_prefixCache.high.back() + klines[i].high);
        g_prefixCache.low.push_back(g_prefixCache.low.back() + klines[i].low);
    }
}

double smaCloseTail(std::size_t n, int period) {
    if (period <= 0 || static_cast<std::size_t>(period) > n) return 0.0;
    const std::size_t start = n - static_cast<std::size_t>(period);
    return (g_prefixCache.close[n] - g_prefixCache.close[start])
         / static_cast<double>(period);
}

double smaHighRange(int startIdx, int endIdx) {
    if (startIdx < 0 || endIdx < startIdx) return 0.0;
    const int period = endIdx - startIdx + 1;
    return (g_prefixCache.high[endIdx + 1] - g_prefixCache.high[startIdx])
         / static_cast<double>(period);
}

double smaLowRange(int startIdx, int endIdx) {
    if (startIdx < 0 || endIdx < startIdx) return 0.0;
    const int period = endIdx - startIdx + 1;
    return (g_prefixCache.low[endIdx + 1] - g_prefixCache.low[startIdx])
         / static_cast<double>(period);
}

std::string fmtPrice(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << v;
    return oss.str();
}

std::string fmtPercent(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << v;
    return oss.str();
}

std::string formatValue(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    std::string text = out.str();
    const auto dotPos = text.find('.');
    if (dotPos != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    if (text.empty() || text == "-0") {
        return "0";
    }
    return text;
}

double getParam(const ParamPoint& params, const std::string& key, double def) {
    auto it = params.find(key);
    return it != params.end() ? it->second : def;
}

std::vector<Kline> extractClosedKlines(const std::vector<Kline>& klines) {
    std::vector<Kline> closed;
    closed.reserve(klines.size());
    for (const auto& k : klines) {
        if (k.isClosed) {
            closed.push_back(k);
        }
    }
    return closed;
}

const std::vector<Kline>& closedKlinesView(
    const std::vector<Kline>& klines,
    std::vector<Kline>& scratch) {
    const bool allClosed = std::all_of(
        klines.begin(), klines.end(), [](const Kline& k) { return k.isClosed; });
    if (allClosed) {
        return klines;
    }
    scratch = extractClosedKlines(klines);
    return scratch;
}

}  // namespace

// -----------------------------------------------------------------------------
// GoldenCrossoverAdapter
// Mirrors plugins/src/golden_crossover/strategy_golden_crossover.cpp
// -----------------------------------------------------------------------------
StrategyParamSpec GoldenCrossoverAdapter::spec(const strategy::StrategyConfig& base) const {
    StrategyParamSpec s;
    s.tunableParams = {"ma_short", "ma_long", "atr_period", "sl_multiplier", "tp_multiplier"};
    s.defaults = {
        {"ma_short",      10,  80,  5,    true },
        {"ma_long",       50, 250, 10,    true },
        {"atr_period",     7,  21,  1,    true },
        {"sl_multiplier", 1.0, 3.0, 0.25, false},
        {"tp_multiplier", 2.0, 5.0, 0.25, false}
    };
    s.constraints = {
        {"ma_short", ParamConstraint::Kind::LessThan, "ma_long"}
    };
    // Plugin-built-in defaults; see note on StrategyParamSpec in
    // optimizable_strategy.h about why we cannot read overridden plugin params.
    s.currentValues = {
        {"ma_short",      50.0},
        {"ma_long",      200.0},
        {"atr_period",    static_cast<double>(base.atrPeriod)},
        {"sl_multiplier", base.slMultiplier},
        {"tp_multiplier", base.tpMultiplier}
    };
    return s;
}

strategy::Signal GoldenCrossoverAdapter::evaluateWith(
    std::string_view symbol,
    std::string_view interval,
    const std::vector<Kline>& klines,
    const ParamPoint& params,
    const strategy::StrategyConfig& baseConfig) const
{
    (void)symbol;
    (void)interval;

    const int maShort = static_cast<int>(getParam(params, "ma_short", 50.0));
    const int maLong  = static_cast<int>(getParam(params, "ma_long", 200.0));
    const int atrPeriod = static_cast<int>(getParam(params, "atr_period",
                                          static_cast<double>(baseConfig.atrPeriod)));

    if (maShort <= 0 || maLong <= maShort || atrPeriod <= 0) return {};

    std::vector<Kline> closedScratch;
    const auto& closed = closedKlinesView(klines, closedScratch);
    const auto minCandles = static_cast<std::size_t>(std::max(maLong, atrPeriod + 1));
    if (closed.size() < minCandles) return {};

    const double atr = strategy::indicators::lastAtr(closed, atrPeriod);
    if (atr <= 0.0) return {};

    ensurePrefixCache(closed);
    const double smaShort = smaCloseTail(closed.size(), maShort);
    const double smaLong  = smaCloseTail(closed.size(), maLong);

    const double spread     = std::abs(smaShort - smaLong) / smaLong;
    const double confidence = std::clamp(spread / 0.01, baseConfig.minConfidence, 1.0);

    if (smaShort > smaLong) {
        return strategy::Signal{
            .direction  = strategy::Signal::Direction::Long,
            .confidence = confidence,
            .atr        = atr,
            .reason     = "Golden Cross: MA" + std::to_string(maShort)
                          + "=" + fmtPrice(smaShort)
                          + " MA" + std::to_string(maLong)
                          + "=" + fmtPrice(smaLong)
                          + " spread=" + fmtPercent(spread * 100.0) + "%",
        };
    }
    if (smaShort < smaLong) {
        return strategy::Signal{
            .direction  = strategy::Signal::Direction::Short,
            .confidence = confidence,
            .atr        = atr,
            .reason     = "Death Cross: MA" + std::to_string(maShort)
                          + "=" + fmtPrice(smaShort)
                          + " MA" + std::to_string(maLong)
                          + "=" + fmtPrice(smaLong)
                          + " spread=" + fmtPercent(spread * 100.0) + "%",
        };
    }
    return {};
}

// -----------------------------------------------------------------------------
// Donchian520CrossoverAdapter
// Mirrors plugins/src/donchian_5_20_crossover/strategy_donchian_5_20_crossover.cpp
// NOTE: despite the strategy name, the plugin uses SMA(short) vs SMA(long), NOT
// Donchian-channel breakouts. This adapter intentionally matches that.
// -----------------------------------------------------------------------------
StrategyParamSpec Donchian520CrossoverAdapter::spec(const strategy::StrategyConfig& base) const {
    StrategyParamSpec s;
    s.tunableParams = {"short_period", "long_period", "atr_period", "sl_multiplier", "tp_multiplier"};
    s.defaults = {
        {"short_period",   5,  30, 5,    true },
        {"long_period",   20, 100, 5,    true },
        {"atr_period",     7,  21, 1,    true },
        {"sl_multiplier", 1.0, 3.0, 0.25, false},
        {"tp_multiplier", 2.0, 5.0, 0.25, false}
    };
    s.constraints = {
        {"short_period", ParamConstraint::Kind::LessThan, "long_period"}
    };
    s.currentValues = {
        {"short_period",   5.0},
        {"long_period",   20.0},
        {"atr_period",    static_cast<double>(base.atrPeriod)},
        {"sl_multiplier", base.slMultiplier},
        {"tp_multiplier", base.tpMultiplier}
    };
    return s;
}

strategy::Signal Donchian520CrossoverAdapter::evaluateWith(
    std::string_view symbol,
    std::string_view interval,
    const std::vector<Kline>& klines,
    const ParamPoint& params,
    const strategy::StrategyConfig& baseConfig) const
{
    (void)symbol;
    (void)interval;

    const int shortPeriod = static_cast<int>(getParam(params, "short_period", 5.0));
    const int longPeriod  = static_cast<int>(getParam(params, "long_period", 20.0));
    const int atrPeriod   = static_cast<int>(getParam(params, "atr_period",
                                            static_cast<double>(baseConfig.atrPeriod)));

    if (shortPeriod <= 0 || longPeriod <= shortPeriod || atrPeriod <= 0) return {};

    std::vector<Kline> closedScratch;
    const auto& closed = closedKlinesView(klines, closedScratch);
    const auto minCandles = static_cast<std::size_t>(std::max(longPeriod, atrPeriod + 1));
    if (closed.size() < minCandles) return {};

    const double atr = strategy::indicators::lastAtr(closed, atrPeriod);
    if (atr <= 0.0) return {};

    ensurePrefixCache(closed);
    const double smaShort = smaCloseTail(closed.size(), shortPeriod);
    const double smaLong  = smaCloseTail(closed.size(), longPeriod);

    if (smaShort > smaLong) {
        return strategy::Signal{
            .direction  = strategy::Signal::Direction::Long,
            .confidence = 1.0,
            .atr        = atr,
            .reason     = "SMA" + std::to_string(shortPeriod) + "=" + fmtPrice(smaShort)
                          + " > SMA" + std::to_string(longPeriod) + "=" + fmtPrice(smaLong),
        };
    }
    if (smaShort < smaLong) {
        return strategy::Signal{
            .direction  = strategy::Signal::Direction::Short,
            .confidence = 1.0,
            .atr        = atr,
            .reason     = "SMA" + std::to_string(shortPeriod) + "=" + fmtPrice(smaShort)
                          + " < SMA" + std::to_string(longPeriod) + "=" + fmtPrice(smaLong),
        };
    }
    return {};
}

// -----------------------------------------------------------------------------
// GartleyDayCrossoverAdapter
// Mirrors plugins/src/gartley_day_crossover/strategy_gartley_day_crossover.cpp
// -----------------------------------------------------------------------------
StrategyParamSpec GartleyDayCrossoverAdapter::spec(const strategy::StrategyConfig& base) const {
    StrategyParamSpec s;
    s.tunableParams = {"fast_period", "slow_period", "offset", "conf_threshold",
                       "atr_period", "sl_multiplier", "tp_multiplier"};
    // Note conf_threshold range: plugin default is 0.02 and the value gates
    // confidence via clamp((bandWidth/mid) / conf_threshold). Reasonable sweep
    // is one order of magnitude around the default (0.005..0.05). The earlier
    // 0.3..0.8 range was a unit error and would always produce confidence ~ 0.
    s.defaults = {
        {"fast_period",     2,   8,  1,     true },
        {"slow_period",     4,  15,  1,     true },
        {"offset",          0,   3,  1,     true },
        {"conf_threshold", 0.005, 0.05, 0.005, false},
        {"atr_period",      7,  21,  1,     true },
        {"sl_multiplier",  1.0, 3.0, 0.25,  false},
        {"tp_multiplier",  2.0, 5.0, 0.25,  false}
    };
    s.constraints = {
        {"fast_period", ParamConstraint::Kind::LessEqual, "slow_period"}
    };
    s.currentValues = {
        {"fast_period",    3.0},
        {"slow_period",    6.0},
        {"offset",         2.0},
        {"conf_threshold", 0.02},
        {"atr_period",     static_cast<double>(base.atrPeriod)},
        {"sl_multiplier",  base.slMultiplier},
        {"tp_multiplier",  base.tpMultiplier}
    };
    return s;
}

strategy::Signal GartleyDayCrossoverAdapter::evaluateWith(
    std::string_view symbol,
    std::string_view interval,
    const std::vector<Kline>& klines,
    const ParamPoint& params,
    const strategy::StrategyConfig& baseConfig) const
{
    (void)symbol;
    (void)baseConfig;  // gartley plugin reads only atrPeriod from base; passed via params

    const int fastPeriod    = static_cast<int>(getParam(params, "fast_period", 3.0));
    const int slowPeriod    = static_cast<int>(getParam(params, "slow_period", 6.0));
    const int offset        = static_cast<int>(getParam(params, "offset", 2.0));
    const double confThresh = getParam(params, "conf_threshold", 0.02);
    const int atrPeriod     = static_cast<int>(getParam(params, "atr_period",
                                              static_cast<double>(baseConfig.atrPeriod)));

    if (fastPeriod <= 0 || slowPeriod <= 0 || offset < 0 || confThresh <= 0.0 || atrPeriod <= 0) {
        return {};
    }

    // The plugin uses klines.size() - 2 as the evaluation index, so the LAST
    // input bar is treated as "forming" (and skipped). To mirror that and remain
    // useful for backtest (where all bars are closed), we pass the same input
    // through unchanged — the formula naturally uses index n-2 as evalIdx.
    const auto minCandles = static_cast<std::size_t>(
        std::max({fastPeriod + 1, 1 + offset + slowPeriod, atrPeriod + 2}));
    if (klines.size() < minCandles) return {};

    const int n       = static_cast<int>(klines.size());
    const int evalIdx = n - 2;

    const std::vector<Kline> closedForAtr(klines.begin(), klines.begin() + evalIdx + 1);
    const double atr = strategy::indicators::lastAtr(closedForAtr, atrPeriod);
    if (atr <= 0.0) return {};

    ensurePrefixCache(klines);

    // fastMA = mean of (high+low)/2 over [evalIdx-fastPeriod+1, evalIdx]
    //        = (sumHigh + sumLow) / (2 * period)
    //        = (smaHighRange + smaLowRange) / 2
    const int fastStartIdx = evalIdx - fastPeriod + 1;
    const double fastMA = (smaHighRange(fastStartIdx, evalIdx)
                         + smaLowRange(fastStartIdx, evalIdx)) / 2.0;

    const int slowEndIdx   = evalIdx - offset;
    const int slowStartIdx = slowEndIdx - slowPeriod + 1;
    if (slowStartIdx < 0) return {};

    const double slowHighMA = smaHighRange(slowStartIdx, slowEndIdx);
    const double slowLowMA  = smaLowRange(slowStartIdx, slowEndIdx);

    const double bandWidth = slowHighMA - slowLowMA;
    const double mid       = (slowHighMA + slowLowMA) / 2.0;
    if (mid <= 0.0) return {};

    const double confidence = std::clamp((bandWidth / mid) / confThresh, 0.0, 1.0);

    if (fastMA > slowHighMA) {
        return strategy::Signal{
            .direction  = strategy::Signal::Direction::Long,
            .confidence = confidence,
            .atr        = atr,
            .reason     = std::string(interval) + " Gartley long: fastMA=" + formatValue(fastMA)
                          + " > slowHighMA=" + formatValue(slowHighMA),
        };
    }
    if (fastMA < slowLowMA) {
        return strategy::Signal{
            .direction  = strategy::Signal::Direction::Short,
            .confidence = confidence,
            .atr        = atr,
            .reason     = std::string(interval) + " Gartley short: fastMA=" + formatValue(fastMA)
                          + " < slowLowMA=" + formatValue(slowLowMA),
        };
    }
    return {};
}

}  // namespace backtest
