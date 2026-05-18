#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace engine {

enum class RiskEquityBasis {
    Margin,
    Wallet,
};

enum class RiskMissingDataMode {
    Open,
    Closed,
};

enum class RiskFailureMode {
    Open,
    Closed,
};

enum class RiskStatus {
    OK,
    SOFT_BREACH,
    HARD_BREACH,
};

struct EquityPoint {
    int64_t id{0};
    int64_t timestampMs{0};
    double equity{0.0};
    int year{0};
    std::string source;
    std::string basis;
};

struct SampledEquityPoint {
    int64_t timestampMs{0};
    double equity{0.0};
};

struct RiskMetricsResult {
    std::string windowKind{"rolling"};
    int64_t windowStartMs{0};
    int64_t windowEndMs{0};
    int64_t computedAtMs{0};
    std::string basis{"margin"};
    int dataPoints{0};
    bool valid{false};

    double annualReturn{0.0};
    double excessReturn{0.0};
    double stdDevAll{0.0};
    double sharpeRatio{0.0};
    double stdDevDownside{0.0};
    double sortinoRatio{0.0};
    double ulcerIndex{0.0};
    // Stored as a negative ratio (e.g., -0.35 for a 35% drawdown).
    double maxDrawdown{0.0};
    double upi{0.0};

    bool isValid() const { return valid; }
};

struct RiskConfig {
    bool enabled{true};
    std::string dbPath{"data/risk_metrics.db"};
    RiskEquityBasis equityBasis{RiskEquityBasis::Margin};
    double riskFreeRate{0.0};
    int minDataPoints{30};
    int sampleIntervalMinutes{60};
    int controlLookbackDays{365};
    int metricsComputeIntervalMinutes{60};
    RiskMissingDataMode missingDataMode{RiskMissingDataMode::Open};
    RiskFailureMode failureMode{RiskFailureMode::Closed};
    double softMaxDrawdown{0.20};
    double hardMaxDrawdown{0.35};
    double softMinUpi{0.5};
    double hardMinUpi{-1.0};

    static RiskConfig fromJson(const nlohmann::json& j);
};

std::string toString(RiskEquityBasis basis);

} // namespace engine
