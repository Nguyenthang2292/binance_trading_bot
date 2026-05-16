// strategy_trend_breakout.cpp
// Strategy: Trend Breakout Trader
// Design: docs/design/strategies/2026-05-15-strategy-trend-breakout-v1.0.md
//
// This strategy trades Donchian breakouts across configured timeframes.

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

const std::vector<std::string> kDefaultIntervals{"30m", "1h", "4h"};
constexpr std::string_view kDefaultTrailingInterval = "4h";

struct TrendBreakoutParams {
    int breakoutPeriod{20};
};

const nlohmann::json& paramsObject(const nlohmann::json& j) {
    static const nlohmann::json empty = nlohmann::json::object();
    if (!j.contains("params") || !j.at("params").is_object()) {
        return empty;
    }
    return j.at("params");
}

TrendBreakoutParams parseParams(const nlohmann::json& j) {
    const auto& params = paramsObject(j);
    TrendBreakoutParams p;
    p.breakoutPeriod = params.value("breakout_period", 20);
    if (p.breakoutPeriod <= 0) {
        p.breakoutPeriod = 20;
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
    const auto& params = paramsObject(j);

    strategy::StrategyConfig cfg;
    cfg.name = j.value("name", "Trend Breakout Trader");
    cfg.type = j.value("type", "trend_breakout");
    cfg.intervals = j.value("intervals", kDefaultIntervals);
    if (cfg.intervals.empty()) {
        cfg.intervals = kDefaultIntervals;
    }
    cfg.scanInterval = std::chrono::seconds(j.value("scan_interval_seconds", 900));
    cfg.maxHoldDuration = std::chrono::seconds(j.value("max_hold_duration_seconds", 604800));
    cfg.riskPct = j.value("risk_pct", 0.01);
    cfg.slMultiplier = j.value("sl_multiplier", 1.5);
    cfg.tpMultiplier = j.value("tp_multiplier", 20.0);
    cfg.minNotional = j.value("min_notional", 1.0);
    cfg.atrPeriod = j.value("atr_period", 14);
    cfg.minConfidence = j.value("min_confidence", 0.5);

    cfg.trailingStop.enabled = params.value("trailing_enabled", j.value("trailing_enabled", true));
    cfg.trailingStop.interval = params.value(
        "trailing_interval",
        j.value("trailing_interval", std::string(kDefaultTrailingInterval)));
    cfg.trailingStop.candles = params.value("trailing_candles", j.value("trailing_candles", 42));
    cfg.trailingStop.checkInterval = std::chrono::seconds(params.value(
        "trailing_check_interval_seconds",
        j.value("trailing_check_interval_seconds", 300)));
    return cfg;
}

class TrendBreakoutStrategy final : public strategy::IStrategy {
public:
    TrendBreakoutStrategy(strategy::StrategyConfig cfg, TrendBreakoutParams params)
        : m_cfg(std::move(cfg)), m_params(params) {}

    const strategy::StrategyConfig& config() const override {
        return m_cfg;
    }

    strategy::Signal evaluate(
        std::string_view symbol,
        std::string_view interval,
        const std::vector<Kline>& klines) const override {
        (void)symbol;

        if (std::find(m_cfg.intervals.begin(), m_cfg.intervals.end(), interval) == m_cfg.intervals.end()) {
            return {};
        }

        const auto minCandles = static_cast<size_t>(m_params.breakoutPeriod + 2);
        if (klines.size() < minCandles) {
            return {};
        }

        const double atr = strategy::indicators::lastAtr(klines, m_cfg.atrPeriod);
        if (atr <= 0.0) {
            return {};
        }

        // Evaluate the most recent closed candle, while klines.back() may still be forming.
        const auto evalIndex = klines.size() - 2;
        const auto& kEval = klines[evalIndex];

        const auto lookbackBegin = klines.begin() + static_cast<std::ptrdiff_t>(evalIndex - m_params.breakoutPeriod);
        const auto lookbackEnd = klines.begin() + static_cast<std::ptrdiff_t>(evalIndex);

        double highestHigh = lookbackBegin->high;
        double lowestLow = lookbackBegin->low;
        for (auto it = lookbackBegin; it != lookbackEnd; ++it) {
            highestHigh = std::max(highestHigh, it->high);
            lowestLow = std::min(lowestLow, it->low);
        }

        if (kEval.close > highestHigh) {
            return strategy::Signal{
                .direction = strategy::Signal::Direction::Long,
                .confidence = 1.0,
                .atr = atr,
                .reason = std::string(interval) + " Donchian breakout long: close=" + formatValue(kEval.close) +
                    " > high20=" + formatValue(highestHigh),
            };
        }

        if (kEval.close < lowestLow) {
            return strategy::Signal{
                .direction = strategy::Signal::Direction::Short,
                .confidence = 1.0,
                .atr = atr,
                .reason = std::string(interval) + " Donchian breakout short: close=" + formatValue(kEval.close) +
                    " < low20=" + formatValue(lowestLow),
            };
        }

        return {};
    }

private:
    strategy::StrategyConfig m_cfg;
    TrendBreakoutParams m_params;
};

} // namespace

extern "C" {

__declspec(dllexport) strategy::IStrategy* createStrategy(const char* config_json) {
    try {
        const auto j = nlohmann::json::parse(config_json == nullptr ? "{}" : config_json);
        auto cfg = parseConfig(j);
        auto params = parseParams(j);
        return new TrendBreakoutStrategy(std::move(cfg), params);
    } catch (...) {
        return nullptr;
    }
}

__declspec(dllexport) void destroyStrategy(strategy::IStrategy* strategy) {
    delete strategy;
}

__declspec(dllexport) const char* strategyType() {
    return "trend_breakout";
}

__declspec(dllexport) const char* pluginVersion() {
    return "1.1.0";
}

} // extern "C"
