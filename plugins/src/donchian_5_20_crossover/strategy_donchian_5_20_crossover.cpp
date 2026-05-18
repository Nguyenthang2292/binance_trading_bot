// strategy_donchian_5_20_crossover.cpp
// Strategy: Donchian 5 and 20-Day Crossover (Crypto MTF State Variant)
// Design: docs/design/2026-05-16-strategy-donchian_5_20_crossover-v1.0.md
//
// Tham khảo: docs/sdk/writing-a-strategy-plugin.md

#include "strategy/istrategy.h"
#include "strategy/strategy_config.h"
#include "strategy/indicators/atr.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// ── Params riêng của strategy ─────────────────────────────────────────────

struct Donchian520Params {
    int shortPeriod{5};
    int longPeriod{20};
};

const std::vector<std::string> kDefaultIntervals{"1d", "4h", "1h", "30m"};

const nlohmann::json& paramsObject(const nlohmann::json& j) {
    static const nlohmann::json empty = nlohmann::json::object();
    if (!j.contains("params") || !j.at("params").is_object()) {
        return empty;
    }
    return j.at("params");
}

Donchian520Params parseParams(const nlohmann::json& j) {
    Donchian520Params p;
    const auto& params = paramsObject(j);
    p.shortPeriod = params.value("short_period", 5);
    p.longPeriod  = params.value("long_period",  20);
    return p;
}

strategy::StrategyConfig parseConfig(const nlohmann::json& j) {
    strategy::StrategyConfig cfg;
    cfg.name            = j.value("name", "Donchian 5 and 20-Day Crossover (Crypto MTF State Variant)");
    cfg.type            = j.value("type", "donchian_5_20_crossover");
    cfg.intervals       = j.value("intervals", kDefaultIntervals);
    if (cfg.intervals.empty()) {
        cfg.intervals = kDefaultIntervals;
    }
    cfg.scanInterval    = std::chrono::seconds(j.value("scan_interval_seconds", 900));
    cfg.maxHoldDuration = std::chrono::seconds(j.value("max_hold_duration_seconds", 86400));
    cfg.riskPct         = j.value("risk_pct", 0.01);
    cfg.slMultiplier    = j.value("sl_multiplier", 1.5);
    cfg.tpMultiplier    = j.value("tp_multiplier", 3.0);
    cfg.takeProfitPercent = j.value("takeProfitPercent", j.value("take_profit_percent", 20.0));
    cfg.minNotional     = j.value("min_notional", 1.0);
    cfg.atrPeriod       = j.value("atr_period", 14);
    cfg.minConfidence   = j.value("min_confidence", 0.5);
    return cfg;
}

void validateConfig(const strategy::StrategyConfig& cfg, const Donchian520Params& params) {
    if (params.shortPeriod <= 0) {
        throw std::invalid_argument("short_period must be > 0");
    }
    if (params.longPeriod <= params.shortPeriod) {
        throw std::invalid_argument("long_period must be > short_period");
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
    if (cfg.minNotional < 0.0) {
        throw std::invalid_argument("min_notional must be >= 0");
    }
    if (cfg.minConfidence < 0.0 || cfg.minConfidence > 1.0) {
        throw std::invalid_argument("min_confidence must be between 0 and 1");
    }
}

// Compute Simple Moving Average over the last `period` closing prices.
static double computeSma(const std::vector<Kline>& klines, int period) {
    const int n = static_cast<int>(klines.size());
    double sum = 0.0;
    for (int i = n - period; i < n; ++i) {
        sum += klines[i].close;
    }
    return sum / static_cast<double>(period);
}

static std::string fmtPrice(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << v;
    return oss.str();
}

// ── Strategy Implementation ───────────────────────────────────────────────

class Donchian520Strategy final : public strategy::IStrategy {
public:
    Donchian520Strategy(strategy::StrategyConfig cfg, Donchian520Params params)
        : m_cfg(std::move(cfg)), m_params(std::move(params)) {}

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

        if (m_params.shortPeriod <= 0 || m_params.longPeriod <= m_params.shortPeriod || m_cfg.atrPeriod <= 0) {
            return {};
        }

        std::vector<Kline> closedKlines;
        closedKlines.reserve(klines.size());
        for (const auto& kline : klines) {
            if (kline.isClosed) {
                closedKlines.push_back(kline);
            }
        }

        const auto minCandles = static_cast<std::size_t>(std::max(m_params.longPeriod, m_cfg.atrPeriod + 1));
        if (closedKlines.size() < minCandles) {
            return {};
        }

        // ── Tính ATR ──────────────────────────────────────────────────────
        const double atr = strategy::indicators::lastAtr(closedKlines, m_cfg.atrPeriod);
        if (atr <= 0.0) {
            return {};
        }

        // ── Tính SMA ──────────────────────────────────────────────────────
        const double smaShort = computeSma(closedKlines, m_params.shortPeriod);
        const double smaLong  = computeSma(closedKlines, m_params.longPeriod);

        // ── Điều kiện Long ────────────────────────────────────────────────
        // SMA(5) > SMA(20): short-term trend mạnh hơn long-term → uptrend
        if (smaShort > smaLong) {
            return strategy::Signal{
                .direction  = strategy::Signal::Direction::Long,
                .confidence = 1.0,
                .atr        = atr,
                .reason     = "SMA" + std::to_string(m_params.shortPeriod) + "=" + fmtPrice(smaShort)
                              + " > SMA" + std::to_string(m_params.longPeriod) + "=" + fmtPrice(smaLong),
            };
        }

        // ── Điều kiện Short ───────────────────────────────────────────────
        // SMA(5) < SMA(20): short-term trend yếu hơn long-term → downtrend
        if (smaShort < smaLong) {
            return strategy::Signal{
                .direction  = strategy::Signal::Direction::Short,
                .confidence = 1.0,
                .atr        = atr,
                .reason     = "SMA" + std::to_string(m_params.shortPeriod) + "=" + fmtPrice(smaShort)
                              + " < SMA" + std::to_string(m_params.longPeriod) + "=" + fmtPrice(smaLong),
            };
        }

        return {};  // smaShort == smaLong: guard case, near impossible with real data
    }

private:
    strategy::StrategyConfig m_cfg;
    Donchian520Params        m_params;
};

} // namespace

// ── C ABI Exports ─────────────────────────────────────────────────────────

extern "C" {

__declspec(dllexport) strategy::IStrategy* createStrategy(const char* config_json) {
    try {
        const auto j = nlohmann::json::parse(config_json == nullptr ? "{}" : config_json);
        auto cfg    = parseConfig(j);
        auto params = parseParams(j);
        validateConfig(cfg, params);
        return new Donchian520Strategy(std::move(cfg), std::move(params));
    } catch (...) {
        return nullptr;
    }
}

__declspec(dllexport) void destroyStrategy(strategy::IStrategy* strategy) {
    delete strategy;
}

__declspec(dllexport) const char* strategyType() {
    return "donchian_5_20_crossover";
}

__declspec(dllexport) const char* pluginVersion() {
    return "1.0.0";
}

} // extern "C"
