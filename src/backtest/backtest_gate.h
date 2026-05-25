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

struct BacktestGateRequest {
    std::string symbol;
    std::string strategyId;        // strategy type key, e.g. "golden_crossover"
    strategy::StrategyConfig baseConfig{};  // copied so the controller's lifetime is independent
    std::string interval;
    std::chrono::system_clock::time_point signalBarOpenTime{};
    strategy::Signal::Direction originalDirection{strategy::Signal::Direction::None};
    double originalAtr{0.0};
    double currentPrice{0.0};
    std::optional<ExchangeSymbol> symbolMeta;
};

enum class DropReason {
    NotEligible,
    InsufficientData,
    GeminiUnavailable,
    GeminiTimeout,
    GeminiInvalidResponse,
    ComboBudgetExhausted,
    NoComboPassedFilter,
    NoPlateauFound,
    MajorityVoteFailed,
    DeadlineExceeded,
    InternalError,
};

struct PassResult {
    strategy::Signal::Direction direction;
    double atr{0.0};
    double initialStopPrice{0.0};
    double slMultiplier{0.0};
    double tpMultiplier{0.0};
    double riskPct{0.0};
    std::unordered_map<std::string, double> optimizedParams;
    double centerSortinoIS{0.0};
    double centerSortinoOOS{0.0};
    int plateauVotePass{0};
    int plateauVoteTotal{0};
    int combosEvaluated{0};
    std::chrono::milliseconds wallTime{};
};

struct DropDetail {
    DropReason reason;
    std::string message;
    int combosEvaluated{0};
    std::chrono::milliseconds wallTime{};
};

using BacktestGateResult = std::variant<PassResult, DropDetail>;

class IBacktestGatePort {
public:
    virtual ~IBacktestGatePort() = default;
    virtual BacktestGateResult evaluate(const BacktestGateRequest& req) const = 0;
};

// Configuration structures for the backtest gate

struct BacktestGateDataConfig {
    std::string historySource{"cache_only"};
    int windowMinCandles{2000};
    int windowMaxCandles{1500};
    int windowSlowestMultiplier{10};
    bool runtimeRestFetchEnabled{false};
    int runtimeRestFetchTimeoutSeconds{10};
    int maxRestRequestsPerSignal{3};
};

struct BacktestGateWalkForwardConfig {
    int folds{4};
    double isFraction{0.75};
    double promptContextFraction{0.50};
    bool signalBarHoldout{true};
};

struct BacktestGateFiltersConfig {
    int minTradesPerFold{10};
    double oosIsRatioThreshold{0.5};
    double minOosSortino{0.3};
};

struct BacktestGatePlateauConfig {
    int neighborhoodRadius{1};
    int maxNeighborhoodSize{81};
    double minPassFraction{0.5};
};

struct BacktestGateVoteConfig {
    double thresholdFraction{0.6};
};

struct BacktestGateBudgetConfig {
    int maxTotalCombos{6000};
};

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

struct BacktestGateCacheConfig {
    int ttlSeconds{7200};
    int maxEntries{256};
};

struct BacktestGateFeeConfig {
    double takerFeeRate{0.0004};
    double slippageBps{0.0};
};

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
