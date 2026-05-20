#include "strategy/indicators/atr.h"
#include "strategy/istrategy.h"
#include "strategy/strategy_config.h"

#include <chrono>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

const std::vector<std::string> kDefaultIntervals{"1d", "4h", "1h", "30m"};

std::string fmtVal(double v) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << v;
    std::string s = out.str();
    const auto dot = s.find('.');
    if (dot != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s.empty() || s == "-0" ? "0" : s;
}

const nlohmann::json& paramsObject(const nlohmann::json& j) {
    static const nlohmann::json empty = nlohmann::json::object();
    if (!j.contains("params") || !j.at("params").is_object()) {
        return empty;
    }
    return j.at("params");
}

struct RicardoRulesParams {
    int swingLookback{3};
    bool fixedTakeProfit{false};
    bool swingTrailingExit{true};
};

RicardoRulesParams parseParams(const nlohmann::json& j) {
    const auto& params = paramsObject(j);
    RicardoRulesParams p;
    p.swingLookback = params.value("swing_lookback", 3);
    if (p.swingLookback < 1) {
        p.swingLookback = 1;
    }
    p.fixedTakeProfit = params.value("fixed_take_profit", false);
    p.swingTrailingExit = params.value("exit_policy", std::string("swing_trailing")) == "swing_trailing";
    return p;
}

strategy::StrategyConfig parseConfig(const nlohmann::json& j) {
    strategy::StrategyConfig cfg;
    cfg.name = j.value("name", "Ricardo Rules");
    cfg.type = j.value("type", "ricardo_rules");
    cfg.intervals = j.value("intervals", kDefaultIntervals);
    if (cfg.intervals.empty()) {
        cfg.intervals = kDefaultIntervals;
    }
    cfg.scanInterval = std::chrono::seconds(j.value("scan_interval_seconds", 900));
    cfg.maxHoldDuration = std::chrono::seconds(j.value("max_hold_duration_seconds", 86400));
    cfg.riskPct = j.value("risk_pct", 0.01);
    cfg.slMultiplier = j.value("sl_multiplier", 1.5);
    cfg.tpMultiplier = j.value("tp_multiplier", 0.0);
    cfg.takeProfitPercent = j.value("takeProfitPercent", j.value("take_profit_percent", 0.0));
    cfg.minNotional = j.value("min_notional", 1.0);
    cfg.atrPeriod = j.value("atr_period", 14);
    if (cfg.atrPeriod <= 0) {
        cfg.atrPeriod = 14;
    }
    cfg.minConfidence = j.value("min_confidence", 0.0);
    return cfg;
}

class RicardoRulesStrategy final : public strategy::IStrategy {
public:
    RicardoRulesStrategy(strategy::StrategyConfig cfg, RicardoRulesParams params)
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

        if (std::find(m_cfg.intervals.begin(), m_cfg.intervals.end(), interval) == m_cfg.intervals.end()) {
            return {};
        }

        // Need ATR window on closed candles and one extra forming candle we ignore.
        const auto minCandles = static_cast<std::size_t>(m_cfg.atrPeriod + 2);
        if (klines.size() < minCandles) {
            return {};
        }

        const std::vector<Kline> closedKlines(klines.begin(), klines.end() - 1);
        if (closedKlines.size() < static_cast<std::size_t>(m_cfg.atrPeriod + 1)) {
            return {};
        }

        const double atr = strategy::indicators::lastAtr(closedKlines, m_cfg.atrPeriod);
        if (atr <= 0.0) {
            return {};
        }

        const Kline& kEval = closedKlines.back();
        const Kline& setupBar = closedKlines[closedKlines.size() - 2];

        if (kEval.close > setupBar.high) {
            const double dist       = kEval.close - setupBar.high;
            const double confidence = std::clamp(dist / atr, 0.0, 1.0);
            const double initialStop = std::min(setupBar.low, kEval.low);
            return strategy::Signal{
                .direction  = strategy::Signal::Direction::Long,
                .confidence = confidence,
                .atr        = atr,
                .reason     = std::string(interval)
                    + " Ricardo breakout long: close=" + fmtVal(kEval.close)
                    + " > prev_high=" + fmtVal(setupBar.high)
                    + " | initial_stop=" + fmtVal(initialStop),
                .initialStopPrice = initialStop,
                .disableFixedTakeProfit = !m_params.fixedTakeProfit,
                .exitPolicy = m_params.swingTrailingExit
                    ? strategy::Signal::ExitPolicy::SwingTrailing
                    : strategy::Signal::ExitPolicy::Default,
                .swingLookback = m_params.swingLookback,
            };
        }

        if (kEval.close < setupBar.low) {
            const double dist       = setupBar.low - kEval.close;
            const double confidence = std::clamp(dist / atr, 0.0, 1.0);
            const double initialStop = std::max(setupBar.high, kEval.high);
            return strategy::Signal{
                .direction  = strategy::Signal::Direction::Short,
                .confidence = confidence,
                .atr        = atr,
                .reason     = std::string(interval)
                    + " Ricardo breakout short: close=" + fmtVal(kEval.close)
                    + " < prev_low=" + fmtVal(setupBar.low)
                    + " | initial_stop=" + fmtVal(initialStop),
                .initialStopPrice = initialStop,
                .disableFixedTakeProfit = !m_params.fixedTakeProfit,
                .exitPolicy = m_params.swingTrailingExit
                    ? strategy::Signal::ExitPolicy::SwingTrailing
                    : strategy::Signal::ExitPolicy::Default,
                .swingLookback = m_params.swingLookback,
            };
        }

        return {};
    }

private:
    strategy::StrategyConfig m_cfg;
    RicardoRulesParams       m_params;
};

} // namespace

extern "C" {

__declspec(dllexport) strategy::IStrategy* createStrategy(const char* config_json) {
    try {
        const auto j = nlohmann::json::parse(config_json == nullptr ? "{}" : config_json);
        auto cfg    = parseConfig(j);
        auto params = parseParams(j);
        return new RicardoRulesStrategy(std::move(cfg), params);
    } catch (...) {
        return nullptr;
    }
}

__declspec(dllexport) void destroyStrategy(strategy::IStrategy* s) {
    delete s;
}

__declspec(dllexport) const char* strategyType() {
    return "ricardo_rules";
}

__declspec(dllexport) const char* pluginVersion() {
    return "1.1.0";
}

} // extern "C"
