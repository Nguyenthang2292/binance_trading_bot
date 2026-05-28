/**
 * @file range_proposer.h
 * @brief Contracts for proposing optimized parameter ranges before grid search.
 */

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

/**
 * @brief Input payload for one range-proposal request.
 */
struct RangeProposalRequest {
    std::string symbol;
    std::string interval;
    std::string strategyId;
    std::vector<std::string> tunableParams;
    std::vector<ParamRange> defaultRanges;
    std::vector<ParamConstraint> constraints;
    std::unordered_map<std::string, double> currentValues;
    int baseAtrPeriod{14};
    double baseMinConfidence{0.0};
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

/**
 * @brief Abstract interface for parameter-range proposal engines.
 */
class IRangeProposer {
public:
    virtual ~IRangeProposer() = default;

    /**
     * @brief Successful proposal payload.
     */
    struct Output {
        std::vector<ParamRange> ranges;
        std::string notes;
    };

    /**
     * @brief Canonical failure reasons used by gate error mapping.
     */
    enum class FailureReason {
        Unavailable,
        Timeout,
        InvalidResponse,
        InternalError,
    };

    /**
     * @brief Failure payload with type and diagnostic text.
     */
    struct Failure {
        FailureReason reason{FailureReason::Unavailable};
        std::string message;
    };

    using Result = std::variant<Output, Failure>;

    /**
     * @brief Proposes constrained ranges for subsequent grid generation.
     */
    virtual Result propose(const RangeProposalRequest& req) = 0;
};

}  // namespace backtest
