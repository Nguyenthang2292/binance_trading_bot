#pragma once

#include "orchestration/qlib_state_store.h"

#include <string>

namespace orchestration {

struct PromotionConfig {
    int minCandles{168};
    double minSharpe{0.5};
    double minHitRate{0.52};
    double minMeanNetReturnBps{0.0};
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
        double meanNetReturnBps{0.0};
    };
    Stats computeStats(const std::string& dbPath, const std::string& modelId, const std::string& interval) const;

private:
    struct EffectiveConfig {
        PromotionConfig config;
        std::string profileName{"default"};
        std::string profileError;
    };

    EffectiveConfig resolveConfig(QlibStateStore& stateStore) const;
    Stats computeStats(
        const std::string& dbPath,
        const std::string& modelId,
        const std::string& interval,
        const PromotionConfig& config) const;
    static double barsPerYear(const std::string& interval);

    PromotionConfig m_config;
};

} // namespace orchestration
