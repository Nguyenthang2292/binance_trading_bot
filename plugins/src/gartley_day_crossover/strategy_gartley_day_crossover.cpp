// strategy_gartley_day_crossover.cpp
// Strategy: Gartley 3&6 Candle Crossover
// Design: docs/design/strategies/2026-05-16-strategy-gartley_day_crossover-v1.0.md
//
// Reference: docs/sdk/writing-a-strategy-plugin.md

#include "strategy/indicators/atr.h"
#include "strategy/istrategy.h"
#include "strategy/strategy_config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct GartleyDayCrossoverParams {
    int fastPeriod{3};
    int slowPeriod{6};
    int offset{2};
    double confThreshold{0.02};
};

const std::vector<std::string> kDefaultIntervals{"1d", "4h", "1h", "30m"};

const nlohmann::json& paramsObject(const nlohmann::json& j) {
    static const nlohmann::json empty = nlohmann::json::object();
    if (!j.contains("params") || !j.at("params").is_object()) {
        return empty;
    }
    return j.at("params");
}

GartleyDayCrossoverParams parseParams(const nlohmann::json& j) {
    const auto& params = paramsObject(j);
    GartleyDayCrossoverParams p;
    p.fastPeriod    = params.value("fast_period",    3);
    p.slowPeriod    = params.value("slow_period",    6);
    p.offset        = params.value("offset",         2);
    p.confThreshold = params.value("conf_threshold", 0.02);
    if (p.fastPeriod <= 0) {
        p.fastPeriod = 3;
    }
    if (p.slowPeriod <= 0) {
        p.slowPeriod = 6;
    }
    if (p.offset < 0) {
        p.offset = 2;
    }
    if (p.confThreshold <= 0.0) {
        p.confThreshold = 0.02;
    }
    return p;
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

strategy::StrategyConfig parseConfig(const nlohmann::json& j) {
    strategy::StrategyConfig cfg;
    cfg.name            = j.value("name", "Gartley 3&6 Candle Crossover");
    cfg.type            = j.value("type", "gartley_day_crossover");
    cfg.intervals       = j.value("intervals", kDefaultIntervals);
    if (cfg.intervals.empty()) {
        cfg.intervals = kDefaultIntervals;
    }
    cfg.scanInterval    = std::chrono::seconds(j.value("scan_interval_seconds",    900));
    cfg.maxHoldDuration = std::chrono::seconds(j.value("max_hold_duration_seconds", 86400));
    cfg.riskPct         = j.value("risk_pct",         0.01);
    cfg.slMultiplier    = j.value("sl_multiplier",    1.5);
    cfg.tpMultiplier    = j.value("tp_multiplier",    3.0);
    cfg.takeProfitPercent = j.value("takeProfitPercent", j.value("take_profit_percent", 20.0));
    cfg.leverage        = j.value("leverage", 10);
    cfg.minNotional     = j.value("min_notional",     1.0);
    cfg.atrPeriod       = j.value("atr_period",       14);
    cfg.minConfidence   = j.value("min_confidence",   0.5);
    return cfg;
}

class GartleyDayCrossoverStrategy final : public strategy::IStrategy {
public:
    GartleyDayCrossoverStrategy(strategy::StrategyConfig cfg, GartleyDayCrossoverParams params)
        : m_cfg(std::move(cfg)), m_params(params) {}

    const strategy::StrategyConfig& config() const override {
        return m_cfg;
    }

    strategy::Signal evaluate(
        std::string_view symbol,
        std::string_view interval,
        const std::vector<Kline>& klines) const override
    {
        (void)symbol;
        (void)interval;

        // minCandles includes one trailing forming candle because evaluation uses size - 2.
        // Ensures: slowStartIdx >= 0 and ATR has atrPeriod + 1 closed candles.
        const auto minCandles = static_cast<std::size_t>(
            std::max({m_params.fastPeriod + 1,
                      1 + m_params.offset + m_params.slowPeriod,
                      m_cfg.atrPeriod + 2}));
        if (klines.size() < minCandles) {
            return {};
        }

        const int n       = static_cast<int>(klines.size());
        const int evalIdx = n - 2;  // most recent closed candle; klines.back() may still be forming

        const std::vector<Kline> closedKlines(klines.begin(), klines.begin() + evalIdx + 1);
        const double atr = strategy::indicators::lastAtr(closedKlines, m_cfg.atrPeriod);
        if (atr <= 0.0) {
            return {};
        }

        // Fast MA: fastPeriod-period SMA of mean=(high+low)/2, ending at evalIdx
        double fastSum = 0.0;
        for (int i = evalIdx - m_params.fastPeriod + 1; i <= evalIdx; ++i) {
            fastSum += (klines[i].high + klines[i].low) / 2.0;
        }
        const double fastMA = fastSum / m_params.fastPeriod;

        // Slow MAs: slowPeriod-period SMA ending at (evalIdx - offset)
        // "offset 2 days" = use the MA value computed 2 days before evalIdx
        const int slowEndIdx   = evalIdx - m_params.offset;
        const int slowStartIdx = slowEndIdx - m_params.slowPeriod + 1;

        double slowHighSum = 0.0;
        double slowLowSum  = 0.0;
        for (int i = slowStartIdx; i <= slowEndIdx; ++i) {
            slowHighSum += klines[i].high;
            slowLowSum  += klines[i].low;
        }
        const double slowHighMA = slowHighSum / m_params.slowPeriod;
        const double slowLowMA  = slowLowSum  / m_params.slowPeriod;

        // Confidence: band width as fraction of mid price, normalized by threshold
        const double bandWidth = slowHighMA - slowLowMA;
        const double mid       = (slowHighMA + slowLowMA) / 2.0;
        if (mid <= 0.0) {
            return {};
        }
        const double confidence = std::clamp(
            (bandWidth / mid) / m_params.confThreshold,
            0.0, 1.0);

        // Long: fast MA breaks above upper band
        if (fastMA > slowHighMA) {
            return strategy::Signal{
                .direction  = strategy::Signal::Direction::Long,
                .confidence = confidence,
                .atr        = atr,
                .reason     = std::string(interval) + " Gartley long: fastMA=" + formatValue(fastMA)
                              + " > slowHighMA=" + formatValue(slowHighMA),
            };
        }

        // Short: fast MA breaks below lower band
        if (fastMA < slowLowMA) {
            return strategy::Signal{
                .direction  = strategy::Signal::Direction::Short,
                .confidence = confidence,
                .atr        = atr,
                .reason     = std::string(interval) + " Gartley short: fastMA=" + formatValue(fastMA)
                              + " < slowLowMA=" + formatValue(slowLowMA),
            };
        }

        return {};  // None: fastMA inside band
    }

private:
    strategy::StrategyConfig      m_cfg;
    GartleyDayCrossoverParams m_params;
};

} // namespace

// C ABI exports required by PluginLoader.

extern "C" {

__declspec(dllexport) strategy::IStrategy* createStrategy(const char* config_json) {
    try {
        const auto j = nlohmann::json::parse(config_json == nullptr ? "{}" : config_json);
        auto cfg     = parseConfig(j);
        auto params  = parseParams(j);
        return new GartleyDayCrossoverStrategy(std::move(cfg), params);
    } catch (...) {
        return nullptr;
    }
}

__declspec(dllexport) void destroyStrategy(strategy::IStrategy* strategy) {
    delete strategy;
}

__declspec(dllexport) const char* strategyType() {
    return "gartley_day_crossover";
}

__declspec(dllexport) const char* pluginVersion() {
    return "1.1.0";
}

} // extern "C"
