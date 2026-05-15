#include "engine/exposure_controller.h"

#include "logger.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace engine {

namespace {

std::string fmt(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

} // namespace

ExposureController::ExposureController(ExposureConfig config, const scanner::KlineCache& cache)
    : m_config(std::move(config)), m_cache(cache) {}

ExposureCheckResult ExposureController::check(
    std::string_view symbol,
    strategy::Signal::Direction direction,
    double proposedNotional,
    const PositionTracker& tracker,
    const account::AccountSnapshot& snapshot,
    double availableBalance) const {
    if (!m_config.enabled) {
        return {ExposureDecision::Allow, 1.0, "exposure control disabled"};
    }
    if (direction == strategy::Signal::Direction::None) {
        return {ExposureDecision::Allow, 1.0, "no-direction signal"};
    }
    if (availableBalance <= 0.0 || proposedNotional <= 0.0) {
        return {ExposureDecision::Allow, 1.0, "balance/notional not positive"};
    }

    const double balanceAbs = std::abs(availableBalance);
    const double softLimit = m_config.softLimitNetBeta * balanceAbs;
    const double hardLimit = m_config.hardLimitNetBeta * balanceAbs;
    const double maxGross = m_config.maxGrossBeta * balanceAbs;
    const double target = m_config.targetNetBeta * balanceAbs;

    const auto metrics = computeMetrics(tracker.all(), snapshot, availableBalance);
    const double beta = getBeta(symbol);
    const double directionSign = direction == strategy::Signal::Direction::Long ? 1.0 : -1.0;
    return decide(
        metrics.netBetaExposure,
        directionSign,
        proposedNotional,
        beta,
        softLimit,
        hardLimit,
        target,
        metrics.grossBetaExposure,
        maxGross);
}

ExposureMetrics ExposureController::currentMetrics(
    const PositionTracker& tracker,
    const account::AccountSnapshot& snapshot,
    double availableBalance) const {
    return computeMetrics(tracker.all(), snapshot, availableBalance);
}

double ExposureController::getBeta(std::string_view symbol) const {
    const auto now = std::chrono::system_clock::now();
    const std::string key(symbol);
    {
        std::lock_guard lock(m_betaCacheMutex);
        const auto it = m_betaCache.find(key);
        if (it != m_betaCache.end() && now - it->second.second <= kBetaCacheTTL) {
            return it->second.first;
        }
    }

    double beta = m_config.defaultBeta;
    bool usedDefault = false;
    const auto calculated = m_betaCalc.calculate(symbol, m_cache, m_config.betaWindowDays);
    if (calculated && std::isfinite(*calculated)) {
        beta = *calculated;
    } else {
        usedDefault = true;
    }

    bool shouldWarn = false;
    {
        std::lock_guard lock(m_betaCacheMutex);
        const auto it = m_betaCache.find(key);
        if (it != m_betaCache.end() && now - it->second.second <= kBetaCacheTTL) {
            return it->second.first;
        }
        m_betaCache[key] = {beta, now};
        if (usedDefault && !m_defaultBetaWarned.contains(key)) {
            m_defaultBetaWarned[key] = true;
            shouldWarn = true;
        }
    }
    if (shouldWarn) {
        Logger::instance().log(
            LogLevel::Warning,
            "exposure default beta fallback symbol=" + key + " beta=" + fmt(m_config.defaultBeta));
    }
    return beta;
}

double ExposureController::getPositionNotional(
    std::string_view symbol,
    const account::AccountSnapshot& snapshot,
    const TrackedPosition& pos) {
    if (snapshot.positions.has_value()) {
        for (const auto& p : *snapshot.positions) {
            if (p.symbol != symbol) {
                continue;
            }
            if (std::abs(p.positionAmt) <= 0.0) {
                continue;
            }
            if (std::abs(p.notional) > 0.0) {
                return std::abs(p.notional);
            }
            if (std::abs(p.markPrice) > 0.0) {
                return std::abs(p.positionAmt * p.markPrice);
            }
            if (std::abs(p.entryPrice) > 0.0) {
                return std::abs(p.positionAmt * p.entryPrice);
            }
        }
    }
    return std::abs(pos.quantity * pos.entryPrice);
}

ExposureMetrics ExposureController::computeMetrics(
    const std::vector<TrackedPosition>& positions,
    const account::AccountSnapshot& snapshot,
    double /*availableBalance*/) const {
    ExposureMetrics out;
    out.positionCount = static_cast<int>(positions.size());
    for (const auto& pos : positions) {
        if (pos.direction == strategy::Signal::Direction::None) {
            continue;
        }
        const double notional = getPositionNotional(pos.symbol, snapshot, pos);
        if (notional <= 0.0) {
            continue;
        }
        const double beta = getBeta(pos.symbol);
        const double weighted = notional * beta;
        if (pos.direction == strategy::Signal::Direction::Long) {
            out.longBetaExposure += weighted;
        } else {
            out.shortBetaExposure += weighted;
        }
        out.grossBetaExposure += std::abs(weighted);
    }

    out.netBetaExposure = out.longBetaExposure - out.shortBetaExposure;
    return out;
}

ExposureCheckResult ExposureController::decide(
    double currentNetBeta,
    double directionSign,
    double proposedNotional,
    double beta,
    double softLimit,
    double hardLimit,
    double target,
    double grossBetaExposure,
    double maxGross) const {
    const double currentDeviation = std::abs(currentNetBeta - target);
    const double newNet = currentNetBeta + directionSign * proposedNotional * beta;
    const double newDeviation = std::abs(newNet - target);
    const double proposedGross = std::abs(proposedNotional * beta);
    const double newGross = grossBetaExposure + proposedGross;

    // Conservative gate: gross limit is evaluated on the full proposed size.
    // We do not attempt to auto-scale a trade just to satisfy gross cap.
    if (newGross > maxGross) {
        return {
            ExposureDecision::Block,
            0.0,
            "gross beta exposure " + fmt(newGross) + " > max " + fmt(maxGross),
        };
    }

    if (newDeviation <= currentDeviation) {
        return {ExposureDecision::Allow, 1.0, "improves net beta deviation"};
    }

    if (newDeviation >= hardLimit) {
        return {
            ExposureDecision::Block,
            0.0,
            "net beta deviation " + fmt(newDeviation) + " >= hard limit " + fmt(hardLimit),
        };
    }

    if (hardLimit > softLimit && newDeviation >= softLimit) {
        double scaleFactor = (hardLimit - newDeviation) / (hardLimit - softLimit);
        scaleFactor = std::clamp(scaleFactor, 0.0, 1.0);
        return {
            ExposureDecision::ScaleDown,
            scaleFactor,
            "net beta deviation " + fmt(newDeviation) + ", scale " + fmt(scaleFactor),
        };
    }

    return {ExposureDecision::Allow, 1.0, "within limits"};
}

} // namespace engine
