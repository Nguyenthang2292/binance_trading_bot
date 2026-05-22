#pragma once

#include "strategy/istrategy.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace orchestration {

enum class ExecutionMode {
    Disabled,
    Shadow,
    ShadowOnly,
    LiveCanary,
    Live,
};

struct RuntimeStateSnapshot {
    bool available{false};
    ExecutionMode mode{ExecutionMode::Disabled};
    std::string modelId;
    std::string interval;
    std::string activeRunId;
    int stateVersion{0};
};

class IExecutionStatePort {
public:
    virtual ~IExecutionStatePort() = default;
    virtual RuntimeStateSnapshot snapshot() const = 0;
    virtual RuntimeStateSnapshot snapshotForAdapter(std::string_view adapterId, std::string_view interval) const {
        (void)adapterId;
        (void)interval;
        return snapshot();
    }
    virtual double canaryRiskMultiplier() const = 0;
};

struct ShadowSignalRecord {
    std::string modelId;
    std::string runId;
    std::string adapterId;
    std::string symbol;
    std::string interval;
    int64_t asofOpenTimeMs{0};
    int64_t capturedAtMs{0};
    strategy::Signal::Direction direction{strategy::Signal::Direction::None};
    double confidence{0.0};
    ExecutionMode executionMode{ExecutionMode::Shadow};
    std::string blockedStage;
    bool wouldPlaceOrder{false};
    double currentPrice{0.0};
    double atr{0.0};
    std::string reason;
};

class IShadowMetricsPort {
public:
    virtual ~IShadowMetricsPort() = default;
    virtual void recordShadowSignal(const ShadowSignalRecord& record) = 0;
};

} // namespace orchestration
