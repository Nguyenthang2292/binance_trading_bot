#pragma once

#include "account/account_snapshot.h"
#include "engine/beta_calculator.h"
#include "engine/position_tracker.h"
#include "strategy/istrategy.h"

#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace engine {

enum class ExposureFailureMode {
    Closed,
    Open,
};

struct ExposureConfig {
    bool enabled{true};
    double targetNetBeta{0.0};
    double softLimitNetBeta{0.5};
    double hardLimitNetBeta{1.0};
    double maxGrossBeta{3.0};
    double defaultBeta{1.0};
    double minNotionalAfterScale{5.0};
    int betaWindowDays{30};
    ExposureFailureMode failureMode{ExposureFailureMode::Closed};
};

struct ExposureMetrics {
    double longBetaExposure{0.0};
    double shortBetaExposure{0.0};
    double netBetaExposure{0.0};
    double grossBetaExposure{0.0};
    int positionCount{0};
};

enum class ExposureDecision {
    Allow,
    ScaleDown,
    Block,
};

struct ExposureCheckResult {
    ExposureDecision decision{ExposureDecision::Allow};
    double scaleFactor{1.0};
    std::string reason;
};

class IExposurePort {
public:
    virtual ~IExposurePort() = default;

    virtual ExposureCheckResult check(
        std::string_view symbol,
        strategy::Signal::Direction direction,
        double proposedNotional,
        const PositionTracker& tracker,
        const account::AccountSnapshot& snapshot,
        double availableBalance) const = 0;

    virtual ExposureMetrics currentMetrics(
        const PositionTracker& tracker,
        const account::AccountSnapshot& snapshot,
        double availableBalance) const = 0;

    virtual ExposureFailureMode failureMode() const = 0;
    virtual double minNotionalAfterScale() const = 0;
};

class NoOpExposurePort final : public IExposurePort {
public:
    ExposureCheckResult check(
        std::string_view,
        strategy::Signal::Direction,
        double,
        const PositionTracker&,
        const account::AccountSnapshot&,
        double) const override {
        return {ExposureDecision::Allow, 1.0, "exposure control disabled"};
    }

    ExposureMetrics currentMetrics(
        const PositionTracker&,
        const account::AccountSnapshot&,
        double) const override {
        return {};
    }

    ExposureFailureMode failureMode() const override {
        return ExposureFailureMode::Open;
    }

    double minNotionalAfterScale() const override {
        return 0.0;
    }
};

class ExposureController final : public IExposurePort {
public:
    ExposureController(ExposureConfig config, const scanner::KlineCache& cache);

    ExposureCheckResult check(
        std::string_view symbol,
        strategy::Signal::Direction direction,
        double proposedNotional,
        const PositionTracker& tracker,
        const account::AccountSnapshot& snapshot,
        double availableBalance) const override;

    ExposureMetrics currentMetrics(
        const PositionTracker& tracker,
        const account::AccountSnapshot& snapshot,
        double availableBalance) const override;

    ExposureFailureMode failureMode() const override {
        return m_config.failureMode;
    }

    double minNotionalAfterScale() const override {
        return m_config.minNotionalAfterScale;
    }

private:
    double getBeta(std::string_view symbol) const;
    static double getPositionNotional(
        std::string_view symbol,
        const account::AccountSnapshot& snapshot,
        const TrackedPosition& pos);
    ExposureMetrics computeMetrics(
        const std::vector<TrackedPosition>& positions,
        const account::AccountSnapshot& snapshot,
        double availableBalance) const;
    ExposureCheckResult decide(
        double currentNetBeta,
        double directionSign,
        double proposedNotional,
        double beta,
        double softLimit,
        double hardLimit,
        double target,
        double grossBetaExposure,
        double maxGross) const;

    ExposureConfig m_config;
    const scanner::KlineCache& m_cache;
    BetaCalculator m_betaCalc;

    mutable std::unordered_map<std::string, std::pair<double, std::chrono::system_clock::time_point>> m_betaCache;
    mutable std::unordered_map<std::string, bool> m_defaultBetaWarned;
    mutable std::mutex m_betaCacheMutex;

    static constexpr std::chrono::hours kBetaCacheTTL{24};
};

} // namespace engine

