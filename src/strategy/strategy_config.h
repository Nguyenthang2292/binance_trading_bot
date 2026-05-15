#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace strategy {

struct StrategyConfig {
    std::string name;
    std::string type;
    std::vector<std::string> intervals;
    std::chrono::seconds scanInterval{3600};
    std::chrono::seconds maxHoldDuration{86400};
    double riskPct{0.01};
    double slMultiplier{1.5};
    double tpMultiplier{3.0};
    double minNotional{1.0};
    int atrPeriod{14};
    double minConfidence{0.0};
};

} // namespace strategy

