#include "strategy/strategy_config.h"

#include <cmath>

namespace strategy {

namespace {

bool finitePositive(double value) {
    return std::isfinite(value) && value > 0.0;
}

bool finiteNonNegative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

} // namespace

compat::expected<void, std::string> validateStrategyConfig(const StrategyConfig& cfg) {
    if (cfg.name.empty()) {
        return compat::unexpected("strategy config missing name");
    }
    if (cfg.type.empty()) {
        return compat::unexpected("strategy config missing type");
    }
    if (cfg.intervals.empty()) {
        return compat::unexpected("strategy config requires at least one interval");
    }
    if (cfg.scanInterval <= std::chrono::seconds::zero()) {
        return compat::unexpected("scanInterval must be > 0 seconds");
    }
    if (cfg.maxHoldDuration <= std::chrono::seconds::zero()) {
        return compat::unexpected("maxHoldDuration must be > 0 seconds");
    }
    if (!finitePositive(cfg.riskPct) || cfg.riskPct > 1.0) {
        return compat::unexpected("riskPct must be finite and in (0,1]");
    }
    if (!finitePositive(cfg.slMultiplier)) {
        return compat::unexpected("slMultiplier must be finite and > 0");
    }
    if (!finiteNonNegative(cfg.tpMultiplier)) {
        return compat::unexpected("tpMultiplier must be finite and >= 0");
    }
    if (!finiteNonNegative(cfg.takeProfitPercent)) {
        return compat::unexpected("takeProfitPercent must be finite and >= 0");
    }
    if (cfg.leverage <= 0 || cfg.leverage > 125) {
        return compat::unexpected("leverage must be in [1,125]");
    }
    if (!finitePositive(cfg.minNotional)) {
        return compat::unexpected("minNotional must be finite and > 0");
    }
    if (cfg.atrPeriod <= 0) {
        return compat::unexpected("atrPeriod must be > 0");
    }
    if (!std::isfinite(cfg.minConfidence) || cfg.minConfidence < 0.0 || cfg.minConfidence > 1.0) {
        return compat::unexpected("minConfidence must be finite and in [0,1]");
    }
    if (cfg.maxConcurrentPositions.has_value() && *cfg.maxConcurrentPositions <= 0) {
        return compat::unexpected("maxConcurrentPositions must be > 0");
    }
    if (cfg.maxTotalRiskPct.has_value() &&
        (!std::isfinite(*cfg.maxTotalRiskPct) || *cfg.maxTotalRiskPct <= 0.0 || *cfg.maxTotalRiskPct > 1.0)) {
        return compat::unexpected("maxTotalRiskPct must be finite and in (0,1]");
    }
    if (cfg.trailingStop.enabled) {
        if (cfg.trailingStop.candles <= 0) {
            return compat::unexpected("trailingStop.candles must be > 0 when trailing is enabled");
        }
        if (cfg.trailingStop.checkInterval <= std::chrono::seconds::zero()) {
            return compat::unexpected("trailingStop.checkInterval must be > 0 when trailing is enabled");
        }
    }

    for (const auto& [interval, duration] : cfg.maxHoldDurationByInterval) {
        if (interval.empty()) {
            return compat::unexpected("maxHoldDurationByInterval contains empty interval key");
        }
        if (duration <= std::chrono::seconds::zero()) {
            return compat::unexpected("maxHoldDurationByInterval duration must be > 0");
        }
    }
    return {};
}

} // namespace strategy
