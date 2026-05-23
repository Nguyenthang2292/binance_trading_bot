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

namespace backtest {

namespace {

double computeSma(const std::vector<Kline>& klines, int period) {
    const int n = static_cast<int>(klines.size());
    double sum = 0.0;
    for (int i = n - period; i < n; ++i) {
        sum += klines[i].close;
    }
    return sum / static_cast<double>(period);
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

    const auto closed = extractClosedKlines(klines);
    const auto minCandles = static_cast<std::size_t>(std::max(maLong, atrPeriod + 1));
    if (closed.size() < minCandles) return {};

    const double atr = strategy::indicators::lastAtr(closed, atrPeriod);
    if (atr <= 0.0) return {};

    const double smaShort = computeSma(closed, maShort);
    const double smaLong  = computeSma(closed, maLong);

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

    const auto closed = extractClosedKlines(klines);
    const auto minCandles = static_cast<std::size_t>(std::max(longPeriod, atrPeriod + 1));
    if (closed.size() < minCandles) return {};

    const double atr = strategy::indicators::lastAtr(closed, atrPeriod);
    if (atr <= 0.0) return {};

    const double smaShort = computeSma(closed, shortPeriod);
    const double smaLong  = computeSma(closed, longPeriod);

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

    double fastSum = 0.0;
    for (int i = evalIdx - fastPeriod + 1; i <= evalIdx; ++i) {
        fastSum += (klines[i].high + klines[i].low) / 2.0;
    }
    const double fastMA = fastSum / fastPeriod;

    const int slowEndIdx   = evalIdx - offset;
    const int slowStartIdx = slowEndIdx - slowPeriod + 1;
    if (slowStartIdx < 0) return {};

    double slowHighSum = 0.0;
    double slowLowSum  = 0.0;
    for (int i = slowStartIdx; i <= slowEndIdx; ++i) {
        slowHighSum += klines[i].high;
        slowLowSum  += klines[i].low;
    }
    const double slowHighMA = slowHighSum / slowPeriod;
    const double slowLowMA  = slowLowSum  / slowPeriod;

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
