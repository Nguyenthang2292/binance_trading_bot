#pragma once

#include "strategy/istrategy.h"

#include <cstdint>
#include <string>

namespace orchestration {

enum class ExecutionMode {
    Disabled,
    Shadow,
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
    virtual double canaryRiskMultiplier() const = 0;
};

struct ShadowSignalRecord {
    std::string modelId;
    std::string runId;
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
