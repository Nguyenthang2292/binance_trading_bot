#pragma once

#include "orchestration/qlib_state_store.h"

#include <string>

namespace orchestration {

struct PromotionConfig {
    int minCandles{168};
    double minSharpe{0.5};
    double minHitRate{0.52};
    int lookbackCandles{336};
};

class PromotionChecker {
public:
    enum class Result {
        NotEnoughData,
        BelowThreshold,
        AlreadyLive,
        PromotedCanary,
        PromotedLive,
    };

    explicit PromotionChecker(PromotionConfig config);

    Result evaluate(QlibStateStore& stateStore);

    struct Stats {
        int outcomes{0};
        double sharpe{0.0};
        double hitRate{0.0};
    };
    Stats computeStats(const std::string& dbPath, const std::string& modelId, const std::string& interval) const;

private:
    static double barsPerYear(const std::string& interval);

    PromotionConfig m_config;
};

} // namespace orchestration
