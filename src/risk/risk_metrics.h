#pragma once

#include "risk/risk_types.h"

#include <chrono>
#include <cstdint>
#include <string_view>
#include <vector>

namespace engine {

class RiskMetrics {
public:
    RiskMetrics(
        double riskFreeRateAnnual = 0.0,
        int minDataPoints = 30,
        std::chrono::minutes sampleInterval = std::chrono::minutes{60});

    RiskMetricsResult compute(
        const std::vector<SampledEquityPoint>& points,
        std::string_view windowKind,
        int64_t windowStartMs,
        int64_t windowEndMs,
        std::string_view basis) const;

private:
    std::vector<double> periodReturns(const std::vector<SampledEquityPoint>& points) const;
    double annualizedReturn(const std::vector<SampledEquityPoint>& points) const;
    double annualizedStdDev(const std::vector<double>& returns) const;
    double annualizedDownsideStdDev(const std::vector<double>& returns) const;
    static double computeUlcerIndex(const std::vector<SampledEquityPoint>& points);
    static double computeMaxDrawdown(const std::vector<SampledEquityPoint>& points);

    double m_riskFreeRate;
    int m_minDataPoints;
    std::chrono::minutes m_sampleInterval;
};

std::vector<SampledEquityPoint> sampleEquity(
    const std::vector<EquityPoint>& raw,
    int64_t windowStartMs,
    int64_t windowEndMs,
    std::chrono::minutes sampleInterval);

} // namespace engine

