/**
 * @file gemini_range_proposer.h
 * @brief Gemini/Python-backed implementation of the range proposer interface.
 */

#pragma once

#include "backtest/backtest_gate.h"
#include "backtest/range_proposer.h"
#include "backtest/walk_forward.h"  // for Partitions

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace backtest {

/**
 * @brief Compact market-context features derived from prompt-context candles.
 */
struct PromptContextAggregates {
    double ret30dPct{0.0};       ///< Return over prompt-context window.
    double atrPctCurrent{0.0};   ///< ATR of last bar as percent of close.
    double avgVolumeUsd{0.0};    ///< Mean volume*close proxy over prompt context.
    std::string trendDirection;  ///< "up", "down", or "flat".
    double realizedVol{0.0};     ///< Std-dev of prompt-context log returns.
    int numCandles{0};           ///< Candle count in prompt context.
};

/**
 * @brief Computes proposer features from prompt-context candles only.
 */
PromptContextAggregates computePromptAggregates(const std::vector<Kline>& promptContext);

/**
 * @brief Range proposer that delegates generation to a Python module.
 *
 * Responsibilities:
 * - Build bounded input JSON from C++ request data.
 * - Execute Python subprocess with timeout and stderr drainage.
 * - Validate and map output into typed C++ ranges.
 * - Cache successful outputs by contextual key.
 */
class GeminiRangeProposer : public IRangeProposer {
public:
    GeminiRangeProposer(BacktestGateGeminiConfig cfg, BacktestGateCacheConfig cacheCfg = {});

    // Computes aggregates from promptContext only; calibration/OOS bars are
    // intentionally excluded to prevent look-ahead leakage.
    Result propose(const RangeProposalRequest& req) override;

    // Explicit overload used by tests/callers that already split partitions.
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
    std::string buildCacheKey(
        const RangeProposalRequest& req,
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

