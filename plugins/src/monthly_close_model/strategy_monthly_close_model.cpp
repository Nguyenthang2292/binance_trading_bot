// strategy_monthly_close_model.cpp
// Strategy: Monthly Close Model (Adapted for MTF)
// Design: docs/design/2026-05-17-strategy-monthly_close_model-v1.0.md
//
// Tham khảo: docs/sdk/writing-a-strategy-plugin.md

#include "strategy/istrategy.h"
#include "strategy/strategy_config.h"
#include "strategy/indicators/atr.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// ── Params riêng của strategy ─────────────────────────────────────────────

struct MonthlyCloseModelParams {
    int period{30};  // số candles mỗi "period" — đồng nhất mọi TF
};

const std::vector<std::string> kDefaultIntervals{"4h", "1h", "30m", "1d"};

const nlohmann::json& paramsObject(const nlohmann::json& j) {
    static const nlohmann::json empty = nlohmann::json::object();
    if (!j.contains("params") || !j.at("params").is_object()) {
        return empty;
    }
    return j.at("params");
}

MonthlyCloseModelParams parseParams(const nlohmann::json& j) {
    MonthlyCloseModelParams p;
    const auto& params = paramsObject(j);
    p.period = params.value("period", 30);
    return p;
}

std::unordered_map<std::string, std::chrono::seconds> parseHoldDurationsByInterval(const nlohmann::json& j) {
    std::unordered_map<std::string, std::chrono::seconds> out;
    const auto field = j.find("max_hold_duration_by_interval_seconds");
    if (field == j.end() || !field->is_object()) {
        return out;
    }

    for (auto it = field->begin(); it != field->end(); ++it) {
        if (!it.value().is_number_integer()) {
            throw std::invalid_argument("max_hold_duration_by_interval_seconds values must be integer seconds");
        }
        out.emplace(it.key(), std::chrono::seconds(it.value().get<int64_t>()));
    }
    return out;
}

strategy::StrategyConfig parseConfig(const nlohmann::json& j) {
    strategy::StrategyConfig cfg;
    cfg.name            = j.value("name", "Monthly Close Model (Adapted MTF)");
    cfg.type            = j.value("type", "monthly_close_model");
    cfg.intervals       = j.value("intervals", kDefaultIntervals);
    if (cfg.intervals.empty()) {
        cfg.intervals = kDefaultIntervals;
    }
    cfg.scanInterval      = std::chrono::seconds(j.value("scan_interval_seconds", 900));
    cfg.maxHoldDuration   = std::chrono::seconds(j.value("max_hold_duration_seconds", 86400));
    cfg.riskPct           = j.value("risk_pct", 0.01);
    cfg.slMultiplier      = j.value("sl_multiplier", 1.5);
    cfg.tpMultiplier      = j.value("tp_multiplier", 3.0);
    cfg.takeProfitPercent = j.value("takeProfitPercent", j.value("take_profit_percent", 20.0));
    cfg.minNotional       = j.value("min_notional", 1.0);
    cfg.atrPeriod         = j.value("atr_period", 14);
    cfg.minConfidence     = j.value("min_confidence", 0.001);
    cfg.maxHoldDurationByInterval = parseHoldDurationsByInterval(j);
    return cfg;
}

void validateConfig(const strategy::StrategyConfig& cfg, const MonthlyCloseModelParams& params) {
    if (params.period <= 0) {
        throw std::invalid_argument("period must be > 0");
    }
    if (cfg.atrPeriod <= 0) {
        throw std::invalid_argument("atr_period must be > 0");
    }
    if (cfg.riskPct <= 0.0) {
        throw std::invalid_argument("risk_pct must be > 0");
    }
    if (cfg.slMultiplier <= 0.0) {
        throw std::invalid_argument("sl_multiplier must be > 0");
    }
    if (cfg.tpMultiplier <= 0.0) {
        throw std::invalid_argument("tp_multiplier must be > 0");
    }
    if (cfg.takeProfitPercent < 0.0) {
        throw std::invalid_argument("takeProfitPercent must be >= 0");
    }
    if (cfg.minConfidence < 0.0 || cfg.minConfidence > 1.0) {
        throw std::invalid_argument("min_confidence must be between 0 and 1");
    }
    for (const auto& [interval, duration] : cfg.maxHoldDurationByInterval) {
        if (interval.empty()) {
            throw std::invalid_argument("max_hold_duration_by_interval_seconds interval must be non-empty");
        }
        if (duration.count() <= 0) {
            throw std::invalid_argument("max_hold_duration_by_interval_seconds values must be > 0");
        }
    }
}

static std::string fmtPct(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << (v * 100.0);
    return oss.str();
}

// ── Strategy Implementation ───────────────────────────────────────────────

class MonthlyCloseModelStrategy final : public strategy::IStrategy {
public:
    MonthlyCloseModelStrategy(strategy::StrategyConfig cfg, MonthlyCloseModelParams params)
        : m_cfg(std::move(cfg)), m_params(std::move(params)) {}

    const strategy::StrategyConfig& config() const override {
        return m_cfg;
    }

    strategy::Signal evaluate(
        std::string_view symbol,
        std::string_view interval,
        const std::vector<Kline>& klines) const override
    {
        // ── Lọc closed candles ────────────────────────────────────────────
        // Partial candle (đang hình thành) có thể làm sai tín hiệu.
        std::vector<Kline> closed;
        closed.reserve(klines.size());
        for (const auto& k : klines) {
            if (k.isClosed) closed.push_back(k);
        }

        // Need enough closed candles for current-vs-period comparison and ATR.
        const auto minCandles = static_cast<std::size_t>(std::max(m_params.period + 1, m_cfg.atrPeriod + 1));
        if (closed.size() < minCandles) {
            return {};
        }

        // ── Tính ATR ──────────────────────────────────────────────────────
        // ATR bắt buộc — engine dùng để sizing, SL và TP fallback.
        const double atr = strategy::indicators::lastAtr(closed, m_cfg.atrPeriod);
        if (atr <= 0.0) {
            return {};
        }

        // ── So sánh current close vs previous period close ────────────────
        const double currentClose = closed.back().close;
        const double prevClose = closed[closed.size() - 1
            - static_cast<std::size_t>(m_params.period)].close;

        if (prevClose <= 0.0) {
            return {};
        }

        const double delta = (currentClose - prevClose) / prevClose;
        const double confidence = std::clamp(std::abs(delta), 0.0, 1.0);

        if (currentClose > prevClose) {
            return strategy::Signal{
                .direction  = strategy::Signal::Direction::Long,
                .confidence = confidence,
                .atr        = atr,
                .reason     = "Period+" + std::to_string(m_params.period)
                              + " close up " + fmtPct(delta) + "%",
            };
        }

        if (currentClose < prevClose) {
            return strategy::Signal{
                .direction  = strategy::Signal::Direction::Short,
                .confidence = confidence,
                .atr        = atr,
                .reason     = "Period+" + std::to_string(m_params.period)
                              + " close dn " + fmtPct(-delta) + "%",
            };
        }

        (void)symbol;
        (void)interval;
        return {};
    }

private:
    strategy::StrategyConfig    m_cfg;
    MonthlyCloseModelParams     m_params;
};

} // namespace

// ── C ABI Exports ─────────────────────────────────────────────────────────

extern "C" {

__declspec(dllexport) strategy::IStrategy* createStrategy(const char* config_json) {
    try {
        const auto j      = nlohmann::json::parse(config_json == nullptr ? "{}" : config_json);
        auto cfg          = parseConfig(j);
        auto params       = parseParams(j);
        validateConfig(cfg, params);
        return new MonthlyCloseModelStrategy(std::move(cfg), std::move(params));
    } catch (...) {
        return nullptr;
    }
}

__declspec(dllexport) void destroyStrategy(strategy::IStrategy* strategy) {
    delete strategy;
}

__declspec(dllexport) const char* strategyType() {
    return "monthly_close_model";
}

__declspec(dllexport) const char* pluginVersion() {
    return "1.0.0";
}

} // extern "C"
