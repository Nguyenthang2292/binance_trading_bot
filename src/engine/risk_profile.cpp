#include "engine/risk_profile.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace engine {

void RiskProfile::validate() const {
    if (riskPct <= 0.0 || riskPct >= 1.0) {
        throw std::invalid_argument("risk profile risk_pct must be > 0 and < 1");
    }
    if (slMultiplier <= 0.0) {
        throw std::invalid_argument("risk profile sl_multiplier must be > 0");
    }
    if (maxPositionNotionalXAvailableBalance <= 0.0) {
        throw std::invalid_argument("risk profile max_position_notional_x_available_balance must be > 0");
    }
    if (maxTotalNotionalPct <= 0.0) {
        throw std::invalid_argument("risk profile max_total_notional_pct must be > 0");
    }
    if (softMaxDrawdown <= 0.0 || hardMaxDrawdown <= 0.0 || softMaxDrawdown > hardMaxDrawdown ||
        hardMaxDrawdown >= 1.0) {
        throw std::invalid_argument("risk profile drawdown thresholds are invalid");
    }
    if (hardMinUpi > softMinUpi) {
        throw std::invalid_argument("risk profile hard_min_upi must be <= soft_min_upi");
    }
    if (softLimitNetBeta < 0.0 || hardLimitNetBeta < 0.0 || softLimitNetBeta > hardLimitNetBeta) {
        throw std::invalid_argument("risk profile net beta thresholds are invalid");
    }
    if (maxGrossBeta <= 0.0) {
        throw std::invalid_argument("risk profile max_gross_beta must be > 0");
    }
}

RiskProfileLoadResult RiskProfileLoadResult::loadActive(const nlohmann::json& root) {
    RiskProfileLoadResult result;
    RiskProfile moderate; // holds the "moderate" defaults as fallback — hardcoded by dev, no need to validate

    if (!root.contains("active_profile")) {
        result.enabled = false; // legacy mode: do not override existing config fields
        return result;
    }

    if (!root.at("active_profile").is_string()) {
        result.enabled = true;
        result.profile = moderate;
        result.warnings.push_back("active_profile must be a string, using moderate");
        return result;
    }

    const std::string activeName = root.at("active_profile").get<std::string>();
    const auto& profiles = root.value("risk_profiles", nlohmann::json::object());

    if (!profiles.is_object() || !profiles.contains(activeName) || !profiles.at(activeName).is_object()) {
        result.enabled = true;
        result.profile = moderate;
        result.warnings.push_back("unknown or missing active risk profile " + activeName + ", using moderate");
        return result;
    }

    try {
        const auto& j = profiles.at(activeName);
        RiskProfile profile;
        profile.name                                  = activeName;
        profile.riskPct                               = j.value("risk_pct",                               moderate.riskPct);
        profile.slMultiplier                          = j.value("sl_multiplier",                          moderate.slMultiplier);
        profile.maxPositionNotionalXAvailableBalance  = j.value("max_position_notional_x_available_balance", moderate.maxPositionNotionalXAvailableBalance);
        profile.maxTotalNotionalPct                   = j.value("max_total_notional_pct",                 moderate.maxTotalNotionalPct);
        profile.softMaxDrawdown                       = j.value("soft_max_drawdown",                      moderate.softMaxDrawdown);
        profile.hardMaxDrawdown                       = j.value("hard_max_drawdown",                      moderate.hardMaxDrawdown);
        profile.softMinUpi                            = j.value("soft_min_upi",                           moderate.softMinUpi);
        profile.hardMinUpi                            = j.value("hard_min_upi",                           moderate.hardMinUpi);
        profile.softLimitNetBeta                      = j.value("soft_limit_net_beta",                    moderate.softLimitNetBeta);
        profile.hardLimitNetBeta                      = j.value("hard_limit_net_beta",                    moderate.hardLimitNetBeta);
        profile.maxGrossBeta                          = j.value("max_gross_beta",                         moderate.maxGrossBeta);
        profile.validate();
        result.profile = profile;
    } catch (const std::exception& e) {
        result.profile = moderate;
        result.warnings.push_back("invalid risk profile " + activeName + ": " + e.what() + "; using moderate");
    }
    result.enabled = true;
    return result;
}

} // namespace engine
