#include "risk/risk_controller.h"

#include "logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace engine {

namespace {

std::string quoteString(std::string_view value) {
    std::ostringstream out;
    out << std::quoted(value);
    return out.str();
}

RiskEquityBasis parseEquityBasis(const std::string& raw) {
    if (raw == "margin") {
        return RiskEquityBasis::Margin;
    }
    if (raw == "wallet") {
        return RiskEquityBasis::Wallet;
    }
    throw std::invalid_argument("risk_analytics.equity_basis must be 'margin' or 'wallet'");
}

RiskMissingDataMode parseMissingDataMode(const std::string& raw) {
    if (raw == "open") {
        return RiskMissingDataMode::Open;
    }
    if (raw == "closed") {
        return RiskMissingDataMode::Closed;
    }
    throw std::invalid_argument("risk_analytics.missing_data_mode must be 'open' or 'closed'");
}

RiskFailureMode parseFailureMode(const std::string& raw) {
    if (raw == "open") {
        return RiskFailureMode::Open;
    }
    if (raw == "closed") {
        return RiskFailureMode::Closed;
    }
    throw std::invalid_argument("risk_analytics.failure_mode must be 'open' or 'closed'");
}

} // namespace

std::string toString(RiskEquityBasis basis) {
    switch (basis) {
        case RiskEquityBasis::Wallet:
            return "wallet";
        case RiskEquityBasis::Margin:
        default:
            return "margin";
    }
}

RiskConfig RiskConfig::fromJson(const nlohmann::json& j) {
    RiskConfig cfg;
    cfg.enabled = j.value("enabled", cfg.enabled);
    cfg.dbPath = j.value("db_path", cfg.dbPath);
    cfg.equityBasis = parseEquityBasis(j.value("equity_basis", std::string("margin")));
    cfg.riskFreeRate = j.value("risk_free_rate", cfg.riskFreeRate);
    cfg.minDataPoints = j.value("min_data_points", cfg.minDataPoints);
    cfg.sampleIntervalMinutes = j.value("sample_interval_minutes", cfg.sampleIntervalMinutes);
    cfg.controlLookbackDays = j.value("control_lookback_days", cfg.controlLookbackDays);
    cfg.metricsComputeIntervalMinutes =
        j.value("metrics_compute_interval_minutes", cfg.metricsComputeIntervalMinutes);
    cfg.missingDataMode = parseMissingDataMode(j.value("missing_data_mode", std::string("open")));
    cfg.failureMode = parseFailureMode(j.value("failure_mode", std::string("closed")));
    cfg.softMaxDrawdown = j.value("soft_max_drawdown", cfg.softMaxDrawdown);
    cfg.hardMaxDrawdown = j.value("hard_max_drawdown", cfg.hardMaxDrawdown);
    cfg.softMinUpi = j.value("soft_min_upi", cfg.softMinUpi);
    cfg.hardMinUpi = j.value("hard_min_upi", cfg.hardMinUpi);

    if (cfg.sampleIntervalMinutes <= 0) {
        throw std::invalid_argument("risk_analytics.sample_interval_minutes must be > 0");
    }
    if (cfg.controlLookbackDays <= 0) {
        throw std::invalid_argument("risk_analytics.control_lookback_days must be > 0");
    }
    if (cfg.metricsComputeIntervalMinutes <= 0) {
        throw std::invalid_argument("risk_analytics.metrics_compute_interval_minutes must be > 0");
    }
    if (cfg.minDataPoints < 2) {
        throw std::invalid_argument("risk_analytics.min_data_points must be >= 2");
    }
    if (cfg.softMaxDrawdown <= 0.0 || cfg.hardMaxDrawdown <= 0.0 || cfg.softMaxDrawdown > cfg.hardMaxDrawdown ||
        cfg.hardMaxDrawdown >= 1.0) {
        throw std::invalid_argument("risk_analytics drawdown thresholds are invalid");
    }
    if (cfg.hardMinUpi > cfg.softMinUpi) {
        throw std::invalid_argument("risk_analytics.hard_min_upi must be <= soft_min_upi");
    }

    return cfg;
}

RiskController::RiskController(RiskDb& db, EquityCurve& curve, RiskMetrics metrics, RiskConfig config)
    : m_db(db),
      m_curve(curve),
      m_metrics(std::move(metrics)),
      m_config(std::move(config)) {}

bool RiskController::canOpenPosition() const {
    if (!m_config.enabled) {
        return true;
    }
    std::shared_lock lock(m_mutex);
    if (m_status == RiskStatus::HARD_BREACH) {
        return false;
    }
    if (!m_latest.isValid()) {
        return m_config.missingDataMode == RiskMissingDataMode::Open;
    }
    return true;
}

void RiskController::onPositionClosed(const account::AccountSnapshot& snapshot, int64_t timestampMs) {
    if (!m_config.enabled) {
        return;
    }
    m_curve.recordTradeClose(selectEquity(snapshot), timestampMs, basisString());
}

void RiskController::onScanCycle(const account::AccountSnapshot& snapshot, int64_t timestampMs) {
    if (!m_config.enabled) {
        return;
    }
    m_curve.recordPeriodic(selectEquity(snapshot), timestampMs, basisString());
}

boost::asio::awaitable<void> RiskController::maybeRecompute(int64_t nowMs) {
    if (!m_config.enabled) {
        co_return;
    }
    {
        std::shared_lock lock(m_mutex);
        const int64_t intervalMs = static_cast<int64_t>(m_config.metricsComputeIntervalMinutes) * 60 * 1000;
        if (intervalMs > 0 && nowMs - m_lastComputeMs < intervalMs) {
            co_return;
        }
    }

    try {
        recomputeMetrics(nowMs);
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::Error, std::string("risk recompute failed: ") + e.what());
        std::unique_lock lock(m_mutex);
        if (m_config.failureMode == RiskFailureMode::Closed) {
            m_status = RiskStatus::HARD_BREACH;
            m_latest.valid = false;
        }
    } catch (...) {
        Logger::instance().log(LogLevel::Error, "risk recompute failed: unknown exception");
        std::unique_lock lock(m_mutex);
        if (m_config.failureMode == RiskFailureMode::Closed) {
            m_status = RiskStatus::HARD_BREACH;
            m_latest.valid = false;
        }
    }

    co_return;
}

RiskStatus RiskController::currentStatus() const {
    std::shared_lock lock(m_mutex);
    return m_status;
}

RiskMetricsResult RiskController::latestMetrics() const {
    std::shared_lock lock(m_mutex);
    return m_latest;
}

void RiskController::recomputeMetrics(int64_t nowMs) {
    const auto [startMs, endMs] = rollingWindow(nowMs);
    const int64_t lookbackMs = static_cast<int64_t>(m_config.controlLookbackDays) * 24 * 60 * 60 * 1000;
    m_db.deleteEquityPointsOlderThan(nowMs - (lookbackMs * 2));
    const auto raw = m_curve.getByTimeRange(basisString(), startMs, endMs);
    const auto sampled = sampleEquity(raw, startMs, endMs, std::chrono::minutes{m_config.sampleIntervalMinutes});
    RiskMetricsResult result = m_metrics.compute(sampled, "rolling", startMs, endMs, basisString());
    result.computedAtMs = nowMs;

    m_db.insertMetrics(result);
    const RiskStatus status = evaluate(result);
    logMetrics(result, status);

    std::unique_lock lock(m_mutex);
    m_latest = result;
    m_status = status;
    m_lastComputeMs = nowMs;
}

RiskStatus RiskController::evaluate(const RiskMetricsResult& metrics) const {
    // maxDrawdown is stored as a negative ratio (e.g., -0.35 for a 35% drawdown).
    const double hardDrawdownLimit = -m_config.hardMaxDrawdown;
    const double softDrawdownLimit = -m_config.softMaxDrawdown;

    if (metrics.maxDrawdown < hardDrawdownLimit) {
        return RiskStatus::HARD_BREACH;
    }
    if (metrics.isValid() && metrics.upi < m_config.hardMinUpi) {
        return RiskStatus::HARD_BREACH;
    }
    if (metrics.maxDrawdown < softDrawdownLimit) {
        return RiskStatus::SOFT_BREACH;
    }
    if (metrics.isValid() && metrics.upi < m_config.softMinUpi) {
        return RiskStatus::SOFT_BREACH;
    }
    return RiskStatus::OK;
}

void RiskController::logMetrics(const RiskMetricsResult& metrics, RiskStatus status) const {
    std::string statusText = "OK";
    LogLevel level = LogLevel::Info;
    if (status == RiskStatus::SOFT_BREACH) {
        statusText = "SOFT_BREACH";
        level = LogLevel::Warning;
    } else if (status == RiskStatus::HARD_BREACH) {
        statusText = "HARD_BREACH";
        level = LogLevel::Error;
    }

    std::ostringstream out;
    out << "risk metrics"
        << " status=" << statusText
        << " basis=" << quoteString(metrics.basis)
        << " points=" << metrics.dataPoints
        << " valid=" << (metrics.valid ? "true" : "false")
        << " annual_return=" << metrics.annualReturn
        << " sharpe=" << metrics.sharpeRatio
        << " sortino=" << metrics.sortinoRatio
        << " ulcer_index=" << metrics.ulcerIndex
        << " max_drawdown=" << metrics.maxDrawdown
        << " upi=" << metrics.upi;
    Logger::instance().log(level, out.str());
}

std::pair<int64_t, int64_t> RiskController::rollingWindow(int64_t nowMs) const {
    const int64_t lookbackMs = static_cast<int64_t>(m_config.controlLookbackDays) * 24 * 60 * 60 * 1000;
    return {nowMs - lookbackMs, nowMs};
}

double RiskController::selectEquity(const account::AccountSnapshot& snapshot) const {
    if (m_config.equityBasis == RiskEquityBasis::Wallet) {
        return snapshot.account.totalWalletBalance;
    }
    return snapshot.account.totalMarginBalance;
}

std::string RiskController::basisString() const {
    return toString(m_config.equityBasis);
}

} // namespace engine
