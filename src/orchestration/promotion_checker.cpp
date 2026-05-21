#include "orchestration/promotion_checker.h"

#include "logger.h"

#include "orchestration/sqlite_helpers.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace orchestration {

PromotionChecker::PromotionChecker(PromotionConfig config)
    : m_config(std::move(config)) {
    m_config.minCandles = std::max(1, m_config.minCandles);
    m_config.lookbackCandles = std::max(1, m_config.lookbackCandles);
}

PromotionChecker::Stats PromotionChecker::computeStats(
    const std::string& dbPath,
    const std::string& modelId,
    const std::string& interval) const {
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
    sqlite3_bind_int(stmt.get(), 3, m_config.lookbackCandles);

    std::vector<double> returns;
    returns.reserve(static_cast<size_t>(m_config.lookbackCandles));
    int hitCount = 0;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const double netReturn = sqlite3_column_double(stmt.get(), 0);
        const int hit = sqlite3_column_int(stmt.get(), 1);
        returns.push_back(netReturn);
        hitCount += (hit != 0 ? 1 : 0);
    }

    out.outcomes = static_cast<int>(returns.size());
    if (returns.empty()) {
        return out;
    }
    out.hitRate = static_cast<double>(hitCount) / static_cast<double>(returns.size());

    double sum = 0.0;
    for (const double r : returns) {
        sum += r;
    }
    const double mean = sum / static_cast<double>(returns.size());

    double sq = 0.0;
    for (const double r : returns) {
        const double d = r - mean;
        sq += d * d;
    }
    const double denom = returns.size() > 1
        ? static_cast<double>(returns.size() - 1)
        : static_cast<double>(returns.size());
    const double stdDev = std::sqrt(sq / denom);
    const double annualizedBars = barsPerYear(interval);
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
    const auto stats = computeStats(stateStore.dbPath(), stateStore.modelId(), stateStore.interval());
    if (stats.outcomes < m_config.minCandles) {
        return Result::NotEnoughData;
    }

    const auto snap = stateStore.snapshot();
    if (snap.mode == ExecutionMode::Live) {
        return Result::AlreadyLive;
    }

    const bool pass = stats.sharpe >= m_config.minSharpe && stats.hitRate >= m_config.minHitRate;
    if (!pass) {
        return Result::BelowThreshold;
    }

    if (snap.mode == ExecutionMode::Shadow) {
        if (stateStore.setExecutionMode(ExecutionMode::LiveCanary)) {
            Logger::instance().log(
                LogLevel::Info,
                "[PHASE4][PROMO] Shadow->LiveCanary sharpe=" + std::to_string(stats.sharpe) +
                    " hit=" + std::to_string(stats.hitRate));
            return Result::PromotedCanary;
        }
        return Result::BelowThreshold;
    }
    if (snap.mode == ExecutionMode::LiveCanary) {
        if (stateStore.setExecutionMode(ExecutionMode::Live)) {
            Logger::instance().log(
                LogLevel::Info,
                "[PHASE4][PROMO] LiveCanary->Live sharpe=" + std::to_string(stats.sharpe) +
                    " hit=" + std::to_string(stats.hitRate));
            return Result::PromotedLive;
        }
        return Result::BelowThreshold;
    }

    return Result::BelowThreshold;
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
