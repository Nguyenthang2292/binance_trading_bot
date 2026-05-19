// strategy_ricardo_rules.cpp
// Strategy: Ricardo Rules
// Design: docs/design/2026-05-19-strategy-ricardo_rules-v1.0.md
//
// TODO: Review evaluate() pseudocode in design doc Section 3 before shipping.
// Tham khảo: docs/sdk/writing-a-strategy-plugin.md

#include "strategy/istrategy.h"
#include "strategy/strategy_config.h"
#include "strategy/indicators/atr.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// ── Helpers ───────────────────────────────────────────────────────────────

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

// ── Params ────────────────────────────────────────────────────────────────

struct RicardoRulesParams {
    // N bars on each side required to qualify a local extreme as a swing point.
    // Reserved for future swing-point trailing stop implementation — not used
    // in evaluate() until engine supports custom trailing stop via plugin ABI.
    int swingLookback{3};
};

RicardoRulesParams parseParams(const nlohmann::json& j) {
    RicardoRulesParams p;
    const auto& params = j.contains("params") && j.at("params").is_object()
        ? j.at("params")
        : nlohmann::json::object();
    p.swingLookback = params.value("swing_lookback", 3);
    if (p.swingLookback < 1) p.swingLookback = 1;
    return p;
}

strategy::StrategyConfig parseConfig(const nlohmann::json& j) {
    strategy::StrategyConfig cfg;
    cfg.name            = j.value("name", "Ricardo Rules");
    cfg.type            = j.value("type", "ricardo_rules");
    cfg.intervals       = j.value("intervals", std::vector<std::string>{"1d", "4h", "1h", "30m"});
    if (cfg.intervals.empty()) cfg.intervals = {"1d", "4h", "1h", "30m"};
    cfg.scanInterval    = std::chrono::seconds(j.value("scan_interval_seconds", 900));
    cfg.maxHoldDuration = std::chrono::seconds(j.value("max_hold_duration_seconds", 86400));
    cfg.riskPct         = j.value("risk_pct", 0.01);
    cfg.slMultiplier    = j.value("sl_multiplier", 1.5);
    cfg.tpMultiplier    = j.value("tp_multiplier", 3.0);
    // takeProfitPercent is Binance Futures PNL/ROI%, not direct price move percent.
    cfg.takeProfitPercent = j.value("takeProfitPercent", j.value("take_profit_percent", 20.0));
    cfg.minNotional     = j.value("min_notional", 1.0);
    cfg.atrPeriod       = j.value("atr_period", 14);
    cfg.minConfidence   = j.value("min_confidence", 0.5);
    return cfg;
}

// ── Strategy ──────────────────────────────────────────────────────────────

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

        // ── Guard ─────────────────────────────────────────────────────────
        // Need: ATR warmup (atrPeriod) + setupBar + kEval + forming bar = atrPeriod + 3
        const auto minCandles = static_cast<std::size_t>(m_cfg.atrPeriod + 3);
        if (klines.size() < minCandles) {
            return {};
        }

        // ── ATR ───────────────────────────────────────────────────────────
        const double atr = strategy::indicators::lastAtr(klines, m_cfg.atrPeriod);
        if (atr <= 0.0) {
            return {};
        }

        // ── Bars ──────────────────────────────────────────────────────────
        // klines.back() may still be forming — evaluate against last two closed bars.
        const std::size_t n = klines.size();
        const Kline& kEval    = klines[n - 2];  // last fully closed candle (entry bar)
        const Kline& setupBar = klines[n - 3];  // bar before kEval (setup bar)

        // ── Long: close breaks above setup bar's high ─────────────────────
        if (kEval.close > setupBar.high) {
            const double dist       = kEval.close - setupBar.high;
            const double confidence = std::clamp(dist / atr, 0.0, 1.0);
            // initial_stop_hint: engine uses ATR-based SL; this is for log reference only
            const double stopHint = std::min(setupBar.low, kEval.low);
            return strategy::Signal{
                .direction  = strategy::Signal::Direction::Long,
                .confidence = confidence,
                .atr        = atr,
                .reason     = std::string(interval)
                    + " Ricardo breakout long: close=" + fmtVal(kEval.close)
                    + " > prev_high=" + fmtVal(setupBar.high)
                    + " | initial_stop_hint=" + fmtVal(stopHint),
            };
        }

        // ── Short: close breaks below setup bar's low ─────────────────────
        if (kEval.close < setupBar.low) {
            const double dist       = setupBar.low - kEval.close;
            const double confidence = std::clamp(dist / atr, 0.0, 1.0);
            const double stopHint = std::max(setupBar.high, kEval.high);
            return strategy::Signal{
                .direction  = strategy::Signal::Direction::Short,
                .confidence = confidence,
                .atr        = atr,
                .reason     = std::string(interval)
                    + " Ricardo breakout short: close=" + fmtVal(kEval.close)
                    + " < prev_low=" + fmtVal(setupBar.low)
                    + " | initial_stop_hint=" + fmtVal(stopHint),
            };
        }

        return {};
    }

private:
    strategy::StrategyConfig m_cfg;
    RicardoRulesParams       m_params;
};

} // namespace

// ── C ABI Exports ─────────────────────────────────────────────────────────

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
    return "1.0.0";
}

} // extern "C"
