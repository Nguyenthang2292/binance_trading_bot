#include "orchestration/promotion_checker.h"

#include "logger.h"

#include "orchestration/sqlite_helpers.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace orchestration {

namespace {

std::string modeToText(ExecutionMode mode) {
    switch (mode) {
        case ExecutionMode::Disabled:
            return "disabled";
        case ExecutionMode::Shadow:
            return "shadow";
        case ExecutionMode::ShadowOnly:
            return "shadow_only";
        case ExecutionMode::LiveCanary:
            return "live_canary";
        case ExecutionMode::Live:
            return "live";
    }
    return "shadow";
}

} // namespace

PromotionChecker::PromotionChecker(PromotionConfig config)
    : m_config(std::move(config)) {
    m_config.minCandles = std::max(1, m_config.minCandles);
    m_config.lookbackCandles = std::max(1, m_config.lookbackCandles);
    m_config.horizonBars = std::max(1, m_config.horizonBars);
    if (!std::isfinite(m_config.minMeanNetReturnBps) || m_config.minMeanNetReturnBps <= 0.0) {
        m_config.minMeanNetReturnBps = 0.1;
    }
}

PromotionChecker::Stats PromotionChecker::computeStats(
    const std::string& dbPath,
    const std::string& modelId,
    const std::string& interval) const {
    return computeStats(dbPath, modelId, interval, m_config);
}

PromotionChecker::Stats PromotionChecker::computeStats(
    const std::string& dbPath,
    const std::string& modelId,
    const std::string& interval,
    const PromotionConfig& config) const {
    Stats out;
    sqlite3* rawDb = nullptr;
    if (sqlite3_open(dbPath.c_str(), &rawDb) != SQLITE_OK || rawDb == nullptr) {
        if (rawDb) {
            sqlite3_close(rawDb);
        }
        throw std::runtime_error("PromotionChecker failed to open sqlite db");
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);
    sqlite_helpers::execOrThrow(db.get(), "PRAGMA busy_timeout=5000;");

    const char* sql =
        "SELECT o.net_return, o.hit "
        "FROM qlib_shadow_outcomes o "
        "JOIN qlib_shadow_signals s ON s.shadow_id = o.shadow_id "
        "WHERE s.model_id = ? "
        "  AND s.interval = ? "
        "  AND s.would_place_order = 1 "
        "ORDER BY o.matured_at_ms DESC "
        "LIMIT ?;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        throw std::runtime_error("PromotionChecker prepare stats query failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    sqlite_helpers::bindText(stmt.get(), 1, modelId);
    sqlite_helpers::bindText(stmt.get(), 2, interval);
    sqlite3_bind_int(stmt.get(), 3, config.lookbackCandles);

    std::vector<double> returns;
    returns.reserve(static_cast<size_t>(config.lookbackCandles));
    int hitCount = 0;
    double sum = 0.0;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const double netReturn = sqlite3_column_double(stmt.get(), 0);
        const int hit = sqlite3_column_int(stmt.get(), 1);
        returns.push_back(netReturn);
        sum += netReturn;
        hitCount += (hit != 0 ? 1 : 0);
    }

    out.outcomes = static_cast<int>(returns.size());
    if (returns.empty()) {
        return out;
    }
    out.hitRate = static_cast<double>(hitCount) / static_cast<double>(returns.size());
    const double mean = sum / static_cast<double>(returns.size());
    out.meanNetReturnBps = mean * 10000.0;

    double sq = 0.0;
    for (const double r : returns) {
        const double d = r - mean;
        sq += d * d;
    }
    const double denom = returns.size() > 1
        ? static_cast<double>(returns.size() - 1)
        : static_cast<double>(returns.size());
    const double stdDev = std::sqrt(sq / denom);
    const double annualizedBars = barsPerYear(interval) / static_cast<double>(std::max(1, config.horizonBars));
    if (!std::isfinite(annualizedBars) || annualizedBars <= 0.0) {
        out.sharpe = std::numeric_limits<double>::quiet_NaN();
        Logger::instance().log(
            LogLevel::Warning,
            "PromotionChecker cannot annualize unsupported interval: " + interval);
        return out;
    }
    if (stdDev <= 1e-12) {
        out.sharpe = 0.0;
    } else {
        out.sharpe = (mean / stdDev) * std::sqrt(annualizedBars);
    }
    return out;
}

PromotionChecker::Result PromotionChecker::evaluate(QlibStateStore& stateStore) {
    const auto effective = resolveConfig(stateStore);
    const auto stats = computeStats(stateStore.dbPath(), stateStore.modelId(), stateStore.interval(), effective.config);
    const auto snap = stateStore.snapshot();

    auto record = [&](std::string decision, std::string reason) {
        stateStore.recordPromotionEvaluation({
            .profileName = effective.profileName,
            .decision = std::move(decision),
            .reason = std::move(reason),
            .executionMode = modeToText(snap.mode),
            .matureSignals = stats.outcomes,
            .candles = stats.outcomes,
            .hitRate = stats.hitRate,
            .sharpe = stats.sharpe,
            .meanNetReturnBps = stats.meanNetReturnBps,
        });
    };

    if (!effective.profileError.empty()) {
        record("profile_error", effective.profileError);
        return Result::BelowThreshold;
    }

    if (stats.outcomes < effective.config.minCandles) {
        std::ostringstream reason;
        reason << "not_enough_data outcomes=" << stats.outcomes
               << " min=" << effective.config.minCandles;
        record("not_enough_data", reason.str());
        return Result::NotEnoughData;
    }

    if (snap.mode == ExecutionMode::Live) {
        record("already_live", "already live");
        return Result::AlreadyLive;
    }

    const bool pass = stats.sharpe >= effective.config.minSharpe &&
        stats.hitRate >= effective.config.minHitRate &&
        stats.meanNetReturnBps >= effective.config.minMeanNetReturnBps;
    if (!pass) {
        std::ostringstream reason;
        reason << "below_threshold sharpe=" << stats.sharpe
               << " min_sharpe=" << effective.config.minSharpe
               << " hit_rate=" << stats.hitRate
               << " min_hit_rate=" << effective.config.minHitRate
               << " mean_net_return_bps=" << stats.meanNetReturnBps
               << " min_mean_net_return_bps=" << effective.config.minMeanNetReturnBps;
        record("below_threshold", reason.str());
        return Result::BelowThreshold;
    }

    if (snap.mode == ExecutionMode::Shadow) {
        if (stateStore.setExecutionMode(ExecutionMode::LiveCanary, {}, "promotion_checker")) {
            Logger::instance().log(
                LogLevel::Info,
                "[PHASE4][PROMO] Shadow->LiveCanary sharpe=" + std::to_string(stats.sharpe) +
                    " hit=" + std::to_string(stats.hitRate));
            record("promoted_canary", "passed thresholds");
            return Result::PromotedCanary;
        }
        record("state_update_failed", "failed to update state shadow->live_canary");
        return Result::BelowThreshold;
    }
    if (snap.mode == ExecutionMode::LiveCanary) {
        if (stateStore.setExecutionMode(ExecutionMode::Live, {}, "promotion_checker")) {
            Logger::instance().log(
                LogLevel::Info,
                "[PHASE4][PROMO] LiveCanary->Live sharpe=" + std::to_string(stats.sharpe) +
                    " hit=" + std::to_string(stats.hitRate));
            record("promoted_live", "passed thresholds");
            return Result::PromotedLive;
        }
        record("state_update_failed", "failed to update state live_canary->live");
        return Result::BelowThreshold;
    }

    record("below_threshold", "mode is not promotable");
    return Result::BelowThreshold;
}

PromotionChecker::EffectiveConfig PromotionChecker::resolveConfig(QlibStateStore& stateStore) const {
    EffectiveConfig effective{.config = m_config};
    effective.config.minCandles = std::max(1, effective.config.minCandles);
    effective.config.lookbackCandles = std::max(1, effective.config.lookbackCandles);
    effective.config.horizonBars = std::max(1, effective.config.horizonBars);
    if (!std::isfinite(effective.config.minMeanNetReturnBps) || effective.config.minMeanNetReturnBps <= 0.0) {
        effective.config.minMeanNetReturnBps = 0.1;
    }
    const auto profileSnapshot = stateStore.promotionProfileNameAndJson();
    effective.profileName = profileSnapshot.profileName;
    if (effective.profileName.empty() || effective.profileName == "default") {
        effective.profileName = "default";
        return effective;
    }

    if (!profileSnapshot.profileJson) {
        effective.profileError = "profile_not_found";
        return effective;
    }
    try {
        const auto profile = nlohmann::json::parse(*profileSnapshot.profileJson);
        if (!profile.is_object()) {
            effective.profileError = "profile_json_not_object";
            return effective;
        }
        const auto readInt = [&](const char* key, int current) {
            return profile.contains(key) && profile.at(key).is_number_integer()
                ? std::max(1, profile.at(key).get<int>())
                : current;
        };
        const auto readDouble = [&](const char* key, double current) {
            return profile.contains(key) && profile.at(key).is_number()
                ? profile.at(key).get<double>()
                : current;
        };
        effective.config.minCandles = readInt("min_candles", effective.config.minCandles);
        effective.config.minCandles = readInt("min_shadow_signals", effective.config.minCandles);
        effective.config.minCandles = readInt("min_mature_signals", effective.config.minCandles);
        effective.config.lookbackCandles = readInt("lookback_candles", effective.config.lookbackCandles);
        effective.config.horizonBars = readInt("horizon_bars", effective.config.horizonBars);
        effective.config.minSharpe = readDouble("min_sharpe", effective.config.minSharpe);
        effective.config.minHitRate = readDouble("min_hit_rate", effective.config.minHitRate);
        effective.config.minMeanNetReturnBps =
            readDouble("min_mean_net_return_bps", effective.config.minMeanNetReturnBps);
        if (!std::isfinite(effective.config.minMeanNetReturnBps) ||
            effective.config.minMeanNetReturnBps <= 0.0) {
            effective.config.minMeanNetReturnBps = 0.1;
        }
    } catch (const std::exception& e) {
        effective.profileError = std::string("profile_json_invalid:") + e.what();
    }
    return effective;
}

double PromotionChecker::barsPerYear(const std::string& interval) {
    if (interval.size() < 2) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const char suffix = interval.back();
    int value = 0;
    const std::string numberText = interval.substr(0, interval.size() - 1);
    const auto* begin = numberText.data();
    const auto* end = numberText.data() + numberText.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value <= 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (suffix == 'm') {
        return (365.0 * 24.0 * 60.0) / static_cast<double>(value);
    }
    if (suffix == 'h') {
        return (365.0 * 24.0) / static_cast<double>(value);
    }
    if (suffix == 'd') {
        return 365.0 / static_cast<double>(value);
    }
    return std::numeric_limits<double>::quiet_NaN();
}

} // namespace orchestration
