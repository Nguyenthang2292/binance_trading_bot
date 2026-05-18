// strategy_golden_crossover.cpp
// Strategy: Golden 50/200 Moving Average Crossover (Crypto MTF State Variant)
// Design: docs/design/2026-05-16-strategy-golden_crossover-v1.0.md
//
// Tham khảo: docs/sdk/writing-a-strategy-plugin.md

#include "strategy/istrategy.h"
#include "strategy/strategy_config.h"
#include "strategy/indicators/atr.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// ── Params riêng của strategy ─────────────────────────────────────────────

struct GoldenCrossoverParams {
    int maShort{50};
    int maLong{200};
};

const std::vector<std::string> kDefaultIntervals{"4h", "1h", "30m"};

const nlohmann::json& paramsObject(const nlohmann::json& j) {
    static const nlohmann::json empty = nlohmann::json::object();
    if (!j.contains("params") || !j.at("params").is_object()) {
        return empty;
    }
    return j.at("params");
}

GoldenCrossoverParams parseParams(const nlohmann::json& j) {
    GoldenCrossoverParams p;
    const auto& params = paramsObject(j);
    p.maShort = params.value("ma_short", 50);
    p.maLong  = params.value("ma_long",  200);
    return p;
}

strategy::StrategyConfig parseConfig(const nlohmann::json& j) {
    strategy::StrategyConfig cfg;
    cfg.name            = j.value("name", "Golden 50/200 Moving Average Crossover (Crypto MTF State Variant)");
    cfg.type            = j.value("type", "golden_crossover");
    cfg.intervals       = j.value("intervals", kDefaultIntervals);
    if (cfg.intervals.empty()) {
        cfg.intervals = kDefaultIntervals;
    }
    cfg.scanInterval    = std::chrono::seconds(j.value("scan_interval_seconds",    900));
    cfg.maxHoldDuration = std::chrono::seconds(j.value("max_hold_duration_seconds", 259200));
    cfg.riskPct         = j.value("risk_pct",       0.01);
    cfg.slMultiplier    = j.value("sl_multiplier",   1.5);
    cfg.tpMultiplier    = j.value("tp_multiplier",   3.0);
    cfg.takeProfitPercent = j.value("takeProfitPercent", j.value("take_profit_percent", 20.0));
    cfg.minNotional     = j.value("min_notional",    1.0);
    cfg.atrPeriod       = j.value("atr_period",      14);
    cfg.minConfidence   = j.value("min_confidence",  0.5);
    return cfg;
}

void validateConfig(const strategy::StrategyConfig& cfg, const GoldenCrossoverParams& params) {
    if (params.maShort <= 0) {
        throw std::invalid_argument("ma_short must be > 0");
    }
    if (params.maLong <= params.maShort) {
        throw std::invalid_argument("ma_long must be > ma_short");
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

static std::string fmtPercent(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << v;
    return oss.str();
}

// ── Strategy Implementation ───────────────────────────────────────────────

class GoldenCrossoverStrategy final : public strategy::IStrategy {
public:
    GoldenCrossoverStrategy(strategy::StrategyConfig cfg, GoldenCrossoverParams params)
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

        if (m_params.maShort <= 0 || m_params.maLong <= m_params.maShort || m_cfg.atrPeriod <= 0) {
            return {};
        }

        // ── Filter: chỉ dùng closed candles ──────────────────────────────
        std::vector<Kline> closed;
        closed.reserve(klines.size());
        for (const auto& k : klines) {
            if (k.isClosed) {
                closed.push_back(k);
            }
        }

        // ── Guard: không đủ candles ───────────────────────────────────────
        // SMA(ma_long) cần đúng ma_long candles. Buffer = 200 → zero margin.
        const auto minCandles = static_cast<std::size_t>(
            std::max(m_params.maLong, m_cfg.atrPeriod + 1));
        if (closed.size() < minCandles) {
            return {};
        }

        // ── Tính ATR ──────────────────────────────────────────────────────
        const double atr = strategy::indicators::lastAtr(closed, m_cfg.atrPeriod);
        if (atr <= 0.0) {
            return {};
        }

        // ── Tính SMA ──────────────────────────────────────────────────────
        const double smaShort = computeSma(closed, m_params.maShort);
        const double smaLong  = computeSma(closed, m_params.maLong);

        // ── Confidence = relative MA spread ───────────────────────────────
        // Spread 1% → confidence 1.0; spread 0.5% → confidence 0.5
        const double spread     = std::abs(smaShort - smaLong) / smaLong;
        const double confidence = std::clamp(spread / 0.01, m_cfg.minConfidence, 1.0);

        // ── Điều kiện Long: Golden Cross state ────────────────────────────
        if (smaShort > smaLong) {
            return strategy::Signal{
                .direction  = strategy::Signal::Direction::Long,
                .confidence = confidence,
                .atr        = atr,
                .reason     = "Golden Cross: MA" + std::to_string(m_params.maShort)
                              + "=" + fmtPrice(smaShort)
                              + " MA" + std::to_string(m_params.maLong)
                              + "=" + fmtPrice(smaLong)
                              + " spread=" + fmtPercent(spread * 100.0) + "%",
            };
        }

        // ── Điều kiện Short: Death Cross state ────────────────────────────
        if (smaShort < smaLong) {
            return strategy::Signal{
                .direction  = strategy::Signal::Direction::Short,
                .confidence = confidence,
                .atr        = atr,
                .reason     = "Death Cross: MA" + std::to_string(m_params.maShort)
                              + "=" + fmtPrice(smaShort)
                              + " MA" + std::to_string(m_params.maLong)
                              + "=" + fmtPrice(smaLong)
                              + " spread=" + fmtPercent(spread * 100.0) + "%",
            };
        }

        return {};  // smaShort == smaLong: near-impossible với real data
    }

private:
    strategy::StrategyConfig  m_cfg;
    GoldenCrossoverParams     m_params;
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
        return new GoldenCrossoverStrategy(std::move(cfg), std::move(params));
    } catch (...) {
        return nullptr;
    }
}

__declspec(dllexport) void destroyStrategy(strategy::IStrategy* strategy) {
    delete strategy;
}

__declspec(dllexport) const char* strategyType() {
    return "golden_crossover";
}

__declspec(dllexport) const char* pluginVersion() {
    return "1.0.0";
}

} // extern "C"
