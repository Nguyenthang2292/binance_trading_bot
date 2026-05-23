#pragma once

#include "backtest/range_proposer.h"
#include "backtest/backtest_gate.h"
#include "backtest/walk_forward.h"  // for Partitions

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace backtest {

// Computes prompt_context aggregates strictly from the promptContext slice.
// Called by GeminiRangeProposer before building the input JSON.
struct PromptContextAggregates {
    double ret30dPct{0.0};       // return over the prompt context window
    double atrPctCurrent{0.0};   // ATR of last bar as % of close
    double avgVolumeUsd{0.0};    // average volume * close (USD proxy)
    std::string trendDirection;  // "up" | "down" | "flat"
    double realizedVol{0.0};     // realized volatility (std of log returns)
    int numCandles{0};
};

PromptContextAggregates computePromptAggregates(const std::vector<Kline>& promptContext);

class GeminiRangeProposer : public IRangeProposer {
public:
    GeminiRangeProposer(BacktestGateGeminiConfig cfg, BacktestGateCacheConfig cacheCfg = {});

    // propose() computes aggregates from `partitions.promptContext` only —
    // calibration / OOS / signal bar are never read.
    Result propose(const RangeProposalRequest& req) override;

    // Overload receiving Partitions to enforce prompt-context isolation.
    Result proposeWithPartitions(
        const RangeProposalRequest& req,
        const std::vector<Kline>& promptContext,
        std::chrono::steady_clock::time_point deadline);

private:
    struct CachedResult {
        Output result;
        std::chrono::steady_clock::time_point expiresAt;
    };

    // Build the input JSON to write on disk for the Python subprocess.
    std::string buildInputJson(
        const RangeProposalRequest& req,
        const PromptContextAggregates& aggs,
        const std::string& evalId) const;

    // Run the Python subprocess and return raw stdout.
    std::string runPythonModule(
        const std::string& inputPath,
        const std::string& outputPath,
        int timeoutSeconds) const;

    // Parse and validate subprocess output.
    Result parseOutput(
        const std::string& rawJson,
        const std::vector<std::string>& tunableParams) const;

    // LRU cache helpers.
    std::string buildCacheKey(const RangeProposalRequest& req,
                              const PromptContextAggregates& aggs) const;
    std::optional<Output> getCached(const std::string& key) const;
    void putCached(const std::string& key, const Output& result) const;
    void evictExpiredLocked() const;

    // Stale eval-dir cleanup (run once at startup).
    void cleanupStaleEvalDirsOnce() const;

    BacktestGateGeminiConfig m_cfg;
    BacktestGateCacheConfig m_cacheCfg;

    mutable std::mutex m_cacheMutex;
    mutable std::unordered_map<std::string, CachedResult> m_cache;

    mutable std::mutex m_cleanupMutex;
    mutable bool m_cleanupDone{false};
};

}  // namespace backtest
