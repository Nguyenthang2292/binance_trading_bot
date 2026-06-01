/**
 * @file backtest_gate.h
 * @brief Public contracts and configuration models for the backtest gate.
 */

#pragma once

#include "strategy/istrategy.h"
#include "types/market.h"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace backtest {

class IOptimizableStrategy;

/**
 * @brief Input payload for a single backtest-gate evaluation.
 */
struct BacktestGateRequest {
    std::string symbol;   ///< Trading symbol (for example, "BTCUSDT").
    std::string strategyId;   ///< Strategy key registered in adapter map.
    strategy::StrategyConfig baseConfig{};   ///< Snapshot of strategy config at evaluation time.
    std::string interval;   ///< Candle interval string (for example, "1h").
    std::chrono::system_clock::time_point signalBarOpenTime{};   ///< Open time of signal bar T.
    strategy::Signal::Direction originalDirection{strategy::Signal::Direction::None};   ///< Live signal direction.
    double originalAtr{0.0};   ///< ATR from the live signal (fallback if recompute fails).
    double currentPrice{0.0};   ///< Current/entry reference price used to derive stop.
    std::optional<ExchangeSymbol> symbolMeta;   ///< Optional tick-size metadata for price rounding.
};

/**
 * @brief Canonical reasons a signal can be dropped by the gate.
 */
enum class DropReason {
    NotEligible,          ///< Strategy has no registered optimizable adapter.
    InsufficientData,     ///< Required history window/folds cannot be constructed.
    GeminiUnavailable,    ///< Range proposer backend unavailable.
    GeminiTimeout,        ///< Range proposer timed out.
    GeminiInvalidResponse,   ///< Range proposer returned invalid payload.
    ComboBudgetExhausted, ///< Parameter grid exceeds configured budget.
    NoComboPassedFilter,  ///< All parameter points failed post-backtest filters.
    NoPlateauFound,       ///< Plateau detector could not produce a robust region.
    SignalMismatch,       ///< Plateau center does not support the requested live signal contract.
    MajorityVoteFailed,   ///< Plateau vote disagreed with live direction.
    DeadlineExceeded,     ///< End-to-end gate deadline was exceeded.
    InternalError,        ///< Unexpected internal failure.
};

/**
 * @brief Successful gate output containing accepted optimized parameters.
 */
struct PassResult {
    strategy::Signal::Direction direction;   ///< Final accepted direction.
    double atr{0.0};   ///< ATR at bar T for the selected center point.
    double initialStopPrice{0.0};   ///< Stop price derived from ATR and SL multiplier.
    double slMultiplier{0.0};   ///< Optimized SL multiplier.
    double tpMultiplier{0.0};   ///< Optimized TP multiplier.
    double riskPct{0.0};   ///< Risk percent carried from base config.
    std::unordered_map<std::string, double> optimizedParams;   ///< Plateau center parameter set.
    double centerSortinoIS{0.0};   ///< Mean in-sample sortino at center.
    double centerSortinoOOS{0.0};  ///< Mean out-of-sample sortino at center.
    int plateauVotePass{0};   ///< Number of vote points matching live direction.
    int plateauVoteTotal{0};  ///< Total vote points considered.
    int combosEvaluated{0};   ///< Number of parameter combos fully evaluated.
    std::chrono::milliseconds wallTime{};   ///< End-to-end gate latency.
};

/**
 * @brief Drop output with machine-readable reason and human-readable context.
 */
struct DropDetail {
    DropReason reason;   ///< Drop reason enum.
    std::string message; ///< Diagnostic message for logs/debugging.
    int combosEvaluated{0};   ///< Number of combos evaluated before dropping.
    std::chrono::milliseconds wallTime{};   ///< End-to-end gate latency until drop.
};

/**
 * @brief Variant return type for gate evaluation.
 */
using BacktestGateResult = std::variant<PassResult, DropDetail>;

/**
 * @brief Abstract port for evaluating live signals through the backtest gate.
 */
class IBacktestGatePort {
public:
    virtual ~IBacktestGatePort() = default;

    /**
     * @brief Runs the complete backtest-gate pipeline for one signal candidate.
     */
    virtual BacktestGateResult evaluate(const BacktestGateRequest& req) const = 0;
};

/// @brief Data-source and historical-window settings.

struct BacktestGateDataConfig {
    std::string historySource{"cache_only"};
    int windowMinCandles{1500};
    int windowMaxCandles{1500};
    int windowSlowestMultiplier{10};
    bool runtimeRestFetchEnabled{false};
    int runtimeRestFetchTimeoutSeconds{10};
    int maxRestRequestsPerSignal{3};
};

/// @brief Walk-forward splitting parameters.
struct BacktestGateWalkForwardConfig {
    int folds{4};
    double isFraction{0.75};
    double promptContextFraction{0.50};
    bool signalBarHoldout{true};
};

/// @brief Fold-level quality filters.
struct BacktestGateFiltersConfig {
    int minTradesPerFold{10};
    double oosIsRatioThreshold{0.5};
    double minOosSortino{0.3};
};

/// @brief Plateau-neighborhood search settings.
struct BacktestGatePlateauConfig {
    int neighborhoodRadius{1};
    int maxNeighborhoodSize{81};
    double minPassFraction{0.5};
};

/// @brief Majority-vote threshold over plateau survivors.
struct BacktestGateVoteConfig {
    double thresholdFraction{0.6};
};

/// @brief Grid-size budget control.
struct BacktestGateBudgetConfig {
    int maxTotalCombos{6000};
};

/// @brief Python proposer runtime settings.
struct BacktestGateGeminiConfig {
    std::string pythonPath{"python"};
    std::string moduleName{"tools.backtest_range_proposer.main"};
    std::string workingDirectory{"."};
    std::string runtimeDir{"tmp/backtest_range_proposer"};
    std::string model{"gemini-3.1-pro-preview"};
    int timeoutSeconds{8};
    int retries{1};
    int staleRuntimeTtlHours{24};
};

/// @brief In-process proposal cache settings.
struct BacktestGateCacheConfig {
    int ttlSeconds{7200};
    int maxEntries{256};
};

/// @brief Backtest fee/slippage assumptions.
struct BacktestGateFeeConfig {
    double takerFeeRate{0.0004};
    double slippageBps{0.0};
};

/**
 * @brief Top-level configuration for backtest gate behavior.
 */
struct BacktestGateConfig {
    bool enabled{false};
    bool shadowOnly{true};
    int workerPoolSize{1};
    int deadlineSeconds{90};
    int maxEvaluationsPerScanCycle{3};
    bool closeGateOnBudgetExhausted{true};
    
    BacktestGateDataConfig data;
    BacktestGateWalkForwardConfig walkForward;
    BacktestGateFiltersConfig filters;
    BacktestGatePlateauConfig plateau;
    BacktestGateVoteConfig vote;
    BacktestGateBudgetConfig budget;
    BacktestGateGeminiConfig gemini;
    BacktestGateCacheConfig cache;
    BacktestGateFeeConfig fee;
};

}  // namespace backtest
