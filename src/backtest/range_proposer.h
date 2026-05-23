#pragma once

#include "backtest/parameter_space.h"
#include "strategy/istrategy.h"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace backtest {

struct RangeProposalRequest {
    std::string symbol;
    std::string interval;
    std::string strategyId;
    std::vector<std::string> tunableParams;
    std::vector<ParamRange> defaultRanges;
    std::vector<ParamConstraint> constraints;
    std::unordered_map<std::string, double> currentValues;
    strategy::Signal::Direction signalDirection{strategy::Signal::Direction::None};
    long long signalBarOpenTimeMs{0};
    int maxTotalCombos{6000};
    // The prompt context summary is opaque to the C++ side — the proposer
    // implementation is free to compute aggregate stats from this slice ONLY.
    // See gemini_range_proposer.cpp for the actual prompt build.
    std::vector<Kline> promptContext;
    // Deadline for the entire proposal operation.  Proposers should respect
    // this and abort if exceeded.  Zero-initialized = no deadline.
    std::chrono::steady_clock::time_point deadline{};
};

class IRangeProposer {
public:
    virtual ~IRangeProposer() = default;
    struct Output {
        std::vector<ParamRange> ranges;
        std::string notes;
    };

    enum class FailureReason {
        Unavailable,
        Timeout,
        InvalidResponse,
        InternalError,
    };

    struct Failure {
        FailureReason reason{FailureReason::Unavailable};
        std::string message;
    };

    using Result = std::variant<Output, Failure>;

    virtual Result propose(const RangeProposalRequest& req) = 0;
};

}  // namespace backtest
