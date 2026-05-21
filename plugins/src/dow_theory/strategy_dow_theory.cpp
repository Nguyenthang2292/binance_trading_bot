// strategy_dow_theory.cpp
// Strategy: Dow Theory
// Design: docs/design/2026-05-19-strategy-dow-theory-v1.0.md (v1.1)

#include "strategy/indicators/atr.h"
#include "strategy/istrategy.h"
#include "strategy/strategy_config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct DowTheoryParams {
    double swingAtrMult{1.5};
};

DowTheoryParams parseParams(const nlohmann::json& j) {
    DowTheoryParams out;
    const auto& params = j.contains("params") ? j.at("params") : nlohmann::json::object();
    out.swingAtrMult = params.value("swing_atr_mult", 1.5);
    if (out.swingAtrMult < 0.5) {
        throw std::invalid_argument("params.swing_atr_mult must be >= 0.5");
    }
    return out;
}

strategy::StrategyConfig parseConfig(const nlohmann::json& j) {
    strategy::StrategyConfig cfg;
    cfg.name = j.value("name", "Dow Theory");
    cfg.type = j.value("type", "dow_theory");
    cfg.intervals = j.value("intervals", std::vector<std::string>{"4h", "1h", "30m"});
    cfg.scanInterval = std::chrono::seconds(j.value("scan_interval_seconds", 900));
    cfg.maxHoldDuration = std::chrono::seconds(j.value("max_hold_duration_seconds", 86400));
    cfg.riskPct = j.value("risk_pct", 0.01);
    cfg.slMultiplier = j.value("sl_multiplier", 1.5);
    cfg.tpMultiplier = j.value("tp_multiplier", 3.0);
    cfg.takeProfitPercent = j.value("takeProfitPercent", j.value("take_profit_percent", 20.0));
    cfg.leverage = j.value("leverage", 10);
    cfg.minNotional = j.value("min_notional", 1.0);
    cfg.atrPeriod = j.value("atr_period", 14);
    cfg.minConfidence = j.value("min_confidence", 0.5);

    if (cfg.intervals.empty()) {
        cfg.intervals = {"4h", "1h", "30m"};
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
    if (cfg.takeProfitPercent < 0.0) {
        throw std::invalid_argument("takeProfitPercent must be >= 0");
    }
    if (cfg.takeProfitPercent == 0.0 && cfg.tpMultiplier <= 0.0) {
        throw std::invalid_argument("tp_multiplier must be > 0 when takeProfitPercent is 0");
    }
    if (cfg.minConfidence < 0.0 || cfg.minConfidence > 1.0) {
        throw std::invalid_argument("min_confidence must be between 0 and 1");
    }
    return cfg;
}

enum class PivotType { High, Low };
enum class TrendDirection { Unknown, Up, Down };

struct Pivot {
    PivotType type;
    double price;
    std::size_t index;
    int64_t openTime;
    std::size_t confirmedAt;
    double atrAtConfirm;
};

std::string fmtPrice(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    std::string s = out.str();
    const auto dot = s.find('.');
    if (dot != std::string::npos) {
        while (!s.empty() && s.back() == '0') {
            s.pop_back();
        }
        if (!s.empty() && s.back() == '.') {
            s.pop_back();
        }
    }
    return (s.empty() || s == "-0") ? "0" : s;
}

void addOrReplacePivot(std::vector<Pivot>& pivots, Pivot candidate) {
    if (pivots.empty()) {
        pivots.push_back(std::move(candidate));
        return;
    }

    Pivot& last = pivots.back();
    if (candidate.type == last.type) {
        const bool stronger = candidate.type == PivotType::High
            ? candidate.price > last.price
            : candidate.price < last.price;
        if (stronger || (candidate.price == last.price && candidate.index > last.index)) {
            last = std::move(candidate);
        }
        return;
    }

    if (candidate.index > last.index) {
        pivots.push_back(std::move(candidate));
    }
}

std::vector<Pivot> detectPivots(
    const std::vector<Kline>& closed,
    const std::vector<double>& atrValues,
    double swingAtrMult) {
    std::vector<Pivot> pivots;
    if (closed.size() < 2 || atrValues.size() != closed.size()) {
        return pivots;
    }

    TrendDirection direction = TrendDirection::Unknown;
    double extremeHigh = closed.front().high;
    std::size_t extremeHighIndex = 0;
    double extremeLow = closed.front().low;
    std::size_t extremeLowIndex = 0;

    for (std::size_t i = 1; i < closed.size(); ++i) {
        const Kline& k = closed[i];
        if (k.high > extremeHigh) {
            extremeHigh = k.high;
            extremeHighIndex = i;
        }
        if (k.low < extremeLow) {
            extremeLow = k.low;
            extremeLowIndex = i;
        }

        const double atrAtI = atrValues[i];
        if (atrAtI <= 0.0) {
            continue;
        }
        const double threshold = swingAtrMult * atrAtI;

        if (direction != TrendDirection::Down &&
            extremeHighIndex < i &&
            (extremeHigh - k.low) >= threshold) {
            addOrReplacePivot(
                pivots,
                Pivot{
                    .type = PivotType::High,
                    .price = extremeHigh,
                    .index = extremeHighIndex,
                    .openTime = closed[extremeHighIndex].openTime,
                    .confirmedAt = i,
                    .atrAtConfirm = atrAtI,
                });
            direction = TrendDirection::Down;
            extremeLow = k.low;
            extremeLowIndex = i;
            continue;
        }

        if (direction != TrendDirection::Up &&
            extremeLowIndex < i &&
            (k.high - extremeLow) >= threshold) {
            addOrReplacePivot(
                pivots,
                Pivot{
                    .type = PivotType::Low,
                    .price = extremeLow,
                    .index = extremeLowIndex,
                    .openTime = closed[extremeLowIndex].openTime,
                    .confirmedAt = i,
                    .atrAtConfirm = atrAtI,
                });
            direction = TrendDirection::Up;
            extremeHigh = k.high;
            extremeHighIndex = i;
            continue;
        }
    }

    return pivots;
}

double clamp01(double value) {
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

class DowTheoryStrategy final : public strategy::IStrategy {
public:
    DowTheoryStrategy(strategy::StrategyConfig cfg, DowTheoryParams params)
        : m_cfg(std::move(cfg)), m_params(std::move(params)) {}

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

        std::vector<Kline> closed;
        closed.reserve(klines.size());
        for (const auto& k : klines) {
            if (k.isClosed) {
                closed.push_back(k);
            }
        }

        constexpr std::size_t kMinClosedCandles = 80;
        if (closed.size() < kMinClosedCandles) {
            return {};
        }

        const std::vector<double> atrValues = strategy::indicators::atr(closed, m_cfg.atrPeriod);
        if (atrValues.empty() || atrValues.back() <= 0.0) {
            return {};
        }
        const double atr = atrValues.back();

        const std::vector<Pivot> pivots = detectPivots(closed, atrValues, m_params.swingAtrMult);

        std::vector<const Pivot*> highs;
        std::vector<const Pivot*> lows;
        highs.reserve(pivots.size());
        lows.reserve(pivots.size());
        for (const auto& p : pivots) {
            if (p.type == PivotType::High) {
                highs.push_back(&p);
            } else {
                lows.push_back(&p);
            }
        }

        if (highs.size() < 2 || lows.size() < 2) {
            return {};
        }

        const Pivot& sh1 = *highs[highs.size() - 2];
        const Pivot& sh2 = *highs[highs.size() - 1];
        const Pivot& sl1 = *lows[lows.size() - 2];
        const Pivot& sl2 = *lows[lows.size() - 1];

        const double close = closed.back().close;
        const double prevClose = closed[closed.size() - 2].close;

        const bool bullStructure =
            sh2.price > sh1.price &&
            sl2.price > sl1.price &&
            sl2.index > sh1.index &&
            sh2.index > sl1.index;

        if (bullStructure && prevClose <= sh2.price && close > sh2.price) {
            const double breakoutStrength = clamp01((close - sh2.price) / atr);
            const double structureQuality = clamp01(
                ((sh2.price - sh1.price) + (sl2.price - sl1.price)) / 2.0 / atr);
            const double confidence = (breakoutStrength + structureQuality) / 2.0;
            return strategy::Signal{
                .direction = strategy::Signal::Direction::Long,
                .confidence = confidence,
                .atr = atr,
                .reason = std::string(interval) +
                    " Dow bull: SH1=" + fmtPrice(sh1.price) +
                    " SH2=" + fmtPrice(sh2.price) +
                    " SL1=" + fmtPrice(sl1.price) +
                    " SL2=" + fmtPrice(sl2.price) +
                    " close=" + fmtPrice(close),
            };
        }

        const bool bearStructure =
            sl2.price < sl1.price &&
            sh2.price < sh1.price &&
            sh2.index > sl1.index &&
            sl2.index > sh1.index;

        if (bearStructure && prevClose >= sl2.price && close < sl2.price) {
            const double breakoutStrength = clamp01((sl2.price - close) / atr);
            const double structureQuality = clamp01(
                ((sl1.price - sl2.price) + (sh1.price - sh2.price)) / 2.0 / atr);
            const double confidence = (breakoutStrength + structureQuality) / 2.0;
            return strategy::Signal{
                .direction = strategy::Signal::Direction::Short,
                .confidence = confidence,
                .atr = atr,
                .reason = std::string(interval) +
                    " Dow bear: SL1=" + fmtPrice(sl1.price) +
                    " SL2=" + fmtPrice(sl2.price) +
                    " SH1=" + fmtPrice(sh1.price) +
                    " SH2=" + fmtPrice(sh2.price) +
                    " close=" + fmtPrice(close),
            };
        }

        return {};
    }

private:
    strategy::StrategyConfig m_cfg;
    DowTheoryParams m_params;
};

} // namespace

extern "C" {

__declspec(dllexport) strategy::IStrategy* createStrategy(const char* config_json) {
    try {
        const auto j = nlohmann::json::parse(config_json == nullptr ? "{}" : config_json);
        auto cfg = parseConfig(j);
        auto params = parseParams(j);
        return new DowTheoryStrategy(std::move(cfg), std::move(params));
    } catch (...) {
        return nullptr;
    }
}

__declspec(dllexport) void destroyStrategy(strategy::IStrategy* strategy) {
    delete strategy;
}

__declspec(dllexport) const char* strategyType() {
    return "dow_theory";
}

__declspec(dllexport) const char* pluginVersion() {
    return "1.1.0";
}

} // extern "C"
