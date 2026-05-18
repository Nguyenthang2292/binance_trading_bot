#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace strategy {

struct TrailingStopConfig {
    bool enabled{false};
    std::string interval;
    int candles{0};
    std::chrono::seconds checkInterval{300};
};

struct StrategyConfig {
    std::string name;
    std::string type;
    std::vector<std::string> intervals;
    std::chrono::seconds scanInterval{3600};
    std::chrono::seconds maxHoldDuration{86400};
    double riskPct{0.01};
    double slMultiplier{1.5};
    double tpMultiplier{3.0};
    double takeProfitPercent{20.0};
    double minNotional{1.0};
    int atrPeriod{14};
    double minConfidence{0.0};
    TrailingStopConfig trailingStop;
    std::unordered_map<std::string, std::chrono::seconds> maxHoldDurationByInterval;
};

} // namespace strategy

