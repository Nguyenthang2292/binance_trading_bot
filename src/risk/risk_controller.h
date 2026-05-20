#pragma once

#include "risk/equity_curve.h"
#include "risk/irisk_port.h"
#include "risk/risk_metrics.h"

#include <shared_mutex>
#include <utility>

namespace engine {

class RiskController final : public IRiskPort {
public:
    RiskController(
        RiskDb& db,
        EquityCurve& curve,
        RiskMetrics metrics,
        RiskConfig config);

    bool canOpenPosition() const override;
    void onPositionClosed(const account::AccountSnapshot& snapshot, int64_t timestampMs) override;
    void onScanCycle(const account::AccountSnapshot& snapshot, int64_t timestampMs) override;
    boost::asio::awaitable<void> maybeRecompute(int64_t nowMs) override;
    RiskStatus currentStatus() const override;

    RiskMetricsResult latestMetrics() const;
    void recomputeMetrics(int64_t nowMs);

private:
    RiskStatus evaluate(const RiskMetricsResult& metrics) const;
    void logMetrics(const RiskMetricsResult& metrics, RiskStatus status, int64_t durationMs) const;
    void handleRecomputeFailure(const std::string& message);
    std::pair<int64_t, int64_t> rollingWindow(int64_t nowMs) const;
    double selectEquity(const account::AccountSnapshot& snapshot) const;
    std::string basisString() const;

    RiskDb& m_db;
    EquityCurve& m_curve;
    RiskMetrics m_metrics;
    RiskConfig m_config;

    mutable std::shared_mutex m_mutex;
    RiskMetricsResult m_latest;
    RiskStatus m_status{RiskStatus::OK};
    int64_t m_lastComputeMs{0};
};

} // namespace engine
