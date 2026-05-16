#pragma once

#include "scanner/kline_cache.h"
#include "strategy/istrategy.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine {

enum class GeminiFilterMode {
    Disabled,
    Enforce,
};

struct GeminiFilterConfig {
    struct QuotaModelLimit {
        std::string model;
        int rpm{0};
        int rpd{0};
    };

    bool enabled{false};
    GeminiFilterMode mode{GeminiFilterMode::Enforce};

    std::string pythonPath{"python"};
    std::string moduleName{"tools.gemini_filter.gemini_filter"};
    std::string workingDirectory{"."};
    std::string runtimeDir{"tmp/gemini_filter"};

    std::string sentimentModel{"gemini-3.1-pro-preview"};
    std::string visionModel{"gemini-3.1-pro-preview"};
    bool modelResolutionEnabled{false};
    std::string modelResolutionMode{"pinned"};
    bool modelResolutionFallbackOnError{true};
    bool modelResolutionAllowPreview{true};
    bool sentimentSearchThenScore{false};

    double sentimentWeight{0.5};
    double visionWeight{0.5};
    double confidenceThreshold{0.6};

    int timeoutSeconds{45};
    int maxEvaluationsPerScanCycle{3};
    int staleRuntimeTtlHours{24};
    int resultCacheTtlSeconds{1800};
    int sentimentCacheTtlSeconds{3600};
    int sentimentCacheMaxStaleSeconds{21600};
    int modelResolutionTtlSeconds{3600};
    int modelResolutionMaxStaleSeconds{86400};
    bool blockOnError{true};
    bool blockOnBudgetExhausted{true};
    bool closeGateOnBudgetExhausted{true};
    bool closeGateOnQuotaExhausted{true};

    bool modelRoutingEnabled{false};
    std::vector<std::string> sentimentModelCandidates{};
    std::vector<std::string> visionModelCandidates{};
    bool visionProEscalationEnabled{false};
    double visionProEscalationMinScore{0.45};
    double visionProEscalationMaxScore{0.65};

    bool quotaEnabled{false};
    double quotaSafetyFactor{0.7};
    int quotaCooldownSecondsOn429{300};
    int quotaDefaultRpm{8};
    int quotaDefaultRpd{250};
    std::vector<QuotaModelLimit> quotaModelLimits{};

    std::vector<std::string> extraTfs{"1h", "4h"};
};

enum class GeminiDecision {
    Allow,
    Block,
};

struct GeminiFilterResult {
    GeminiDecision decision{GeminiDecision::Block};
    double confidence{0.0};
    double sentimentScore{0.0};
    double visionScore{0.0};
    std::string reason;
    std::string errorCode;
    bool hasError{false};
};

class IGeminiFilterPort {
public:
    virtual ~IGeminiFilterPort() = default;

    virtual GeminiFilterResult evaluate(
        std::string_view symbol,
        strategy::Signal::Direction direction,
        std::string_view signalInterval,
        const scanner::KlineCache& cache) const = 0;
};

class NoOpGeminiFilterPort final : public IGeminiFilterPort {
public:
    GeminiFilterResult evaluate(
        std::string_view,
        strategy::Signal::Direction,
        std::string_view,
        const scanner::KlineCache&) const override {
        return {GeminiDecision::Allow, 1.0, 1.0, 1.0, "gemini filter disabled", {}, false};
    }
};

class GeminiFilterController final : public IGeminiFilterPort {
public:
    explicit GeminiFilterController(GeminiFilterConfig config);

    GeminiFilterResult evaluate(
        std::string_view symbol,
        strategy::Signal::Direction direction,
        std::string_view signalInterval,
        const scanner::KlineCache& cache) const override;

private:
    struct CachedResult {
        GeminiFilterResult result;
        std::chrono::steady_clock::time_point expiresAt;
    };

    GeminiFilterResult buildBlockResult(std::string reason, std::string errorCode = "gemini_error") const;
    void cleanupStaleEvalDirsOnce() const;
    std::string runSubprocess(const std::string& inputPath) const;
    std::optional<GeminiFilterResult> getCachedResult(
        std::string_view symbol,
        strategy::Signal::Direction direction,
        std::string_view signalInterval,
        const scanner::KlineCache& cache) const;
    void putCachedResult(
        std::string_view symbol,
        strategy::Signal::Direction direction,
        std::string_view signalInterval,
        const scanner::KlineCache& cache,
        const GeminiFilterResult& result) const;
    std::string buildCacheKey(
        std::string_view symbol,
        strategy::Signal::Direction direction,
        std::string_view signalInterval,
        const scanner::KlineCache& cache) const;
    void pruneExpiredCacheEntriesLocked(std::chrono::steady_clock::time_point now) const;

    GeminiFilterConfig m_config;
    mutable std::mutex m_staleCleanupMutex;
    mutable bool m_staleCleanupDone{false};
    mutable std::mutex m_cacheMutex;
    mutable std::unordered_map<std::string, CachedResult> m_resultCache;
    mutable size_t m_cacheHitCount{0};
    mutable size_t m_cacheMissCount{0};
};

} // namespace engine
