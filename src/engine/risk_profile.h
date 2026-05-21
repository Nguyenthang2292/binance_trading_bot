#pragma once

#include <nlohmann/json_fwd.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine {

struct RiskProfile {
    std::string name{"moderate"};

    // sizing (applies to all strategies)
    double riskPct{0.01};
    double slMultiplier{1.5};

    // engine guardrails
    double maxPositionNotionalXAvailableBalance{0.5};

    // order cap
    double maxTotalNotionalPct{8.0};

    // risk analytics
    double softMaxDrawdown{0.20};
    double hardMaxDrawdown{0.35};
    double softMinUpi{0.5};
    double hardMinUpi{-1.0};

    // exposure control
    double softLimitNetBeta{0.5};
    double hardLimitNetBeta{1.0};
    double maxGrossBeta{3.0};

    void validate() const;
};

struct RiskProfileLoadResult {
    bool enabled{false};
    RiskProfile profile{};
    std::vector<std::string> warnings;

    // Load active profile from root config JSON.
    // If active_profile is absent, profile mode is disabled and legacy config stays authoritative.
    // If active_profile is present but invalid/unknown, fall back to the hardcoded moderate profile.
    static RiskProfileLoadResult loadActive(const nlohmann::json& root);
};

} // namespace engine
