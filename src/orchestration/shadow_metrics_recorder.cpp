#include "orchestration/shadow_metrics_recorder.h"

#include "logger.h"
#include "orchestration/sqlite_helpers.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace orchestration {

namespace {

using sqlite_helpers::bindText;
using sqlite_helpers::columnText;
using sqlite_helpers::execOrThrow;

std::string makeShadowId(
    std::string_view modelId,
    std::string_view symbol,
    std::string_view interval,
    int64_t asofOpenMs,
    int64_t capturedAtMs) {
    std::ostringstream out;
    out << modelId << "|" << symbol << "|" << interval << "|" << asofOpenMs << "|" << capturedAtMs;
    return out.str();
}

} // namespace

ShadowMetricsRecorder::ShadowMetricsRecorder(ShadowMetricsConfig config)
    : m_config(std::move(config)) {
    const std::filesystem::path path(m_config.dbPath);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    if (sqlite3_open(m_config.dbPath.c_str(), &m_db) != SQLITE_OK || m_db == nullptr) {
        const std::string message = m_db ? sqlite3_errmsg(m_db) : "sqlite3_open failed";
        if (m_db) {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
        throw std::runtime_error("ShadowMetricsRecorder open failed: " + message);
    }
    execOrThrow(m_db, "PRAGMA journal_mode=WAL;");
    execOrThrow(m_db, "PRAGMA synchronous=NORMAL;");
    execOrThrow(m_db, "PRAGMA busy_timeout=5000;");
}

ShadowMetricsRecorder::~ShadowMetricsRecorder() {
    if (m_db) {
        finalizeStatements();
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

void ShadowMetricsRecorder::initializeSchema() {
    std::lock_guard<std::mutex> lock(m_mutex);
    execOrThrow(
        m_db,
        "CREATE TABLE IF NOT EXISTS qlib_candles ("
        "symbol TEXT NOT NULL,"
        "interval TEXT NOT NULL,"
        "open_time_ms INTEGER NOT NULL,"
        "close_time_ms INTEGER NOT NULL,"
        "open REAL NOT NULL,"
        "high REAL NOT NULL,"
        "low REAL NOT NULL,"
        "close REAL NOT NULL,"
        "volume REAL NOT NULL,"
        "quote_volume REAL,"
        "trade_count INTEGER,"
        "inserted_at_ms INTEGER NOT NULL,"
        "PRIMARY KEY (symbol, interval, open_time_ms)"
        ");");

    execOrThrow(
        m_db,
        "CREATE TABLE IF NOT EXISTS qlib_actual_returns ("
        "symbol TEXT NOT NULL,"
        "interval TEXT NOT NULL,"
        "asof_open_time_ms INTEGER NOT NULL,"
        "horizon_bars INTEGER NOT NULL,"
        "exit_open_time_ms INTEGER NOT NULL,"
        "entry_close REAL NOT NULL,"
        "exit_close REAL NOT NULL,"
        "raw_return REAL NOT NULL,"
        "computed_at_ms INTEGER NOT NULL,"
        "PRIMARY KEY (symbol, interval, asof_open_time_ms, horizon_bars)"
        ");");
    execOrThrow(
        m_db,
        "CREATE INDEX IF NOT EXISTS idx_qlib_candles_interval_open "
        "ON qlib_candles (interval, open_time_ms);");
    execOrThrow(
        m_db,
        "CREATE INDEX IF NOT EXISTS idx_qlib_actual_returns_interval_asof "
        "ON qlib_actual_returns (interval, asof_open_time_ms);");

    execOrThrow(
        m_db,
        "CREATE TABLE IF NOT EXISTS qlib_shadow_signals ("
        "shadow_id TEXT PRIMARY KEY,"
        "model_id TEXT NOT NULL,"
        "run_id TEXT,"
        "symbol TEXT NOT NULL,"
        "interval TEXT NOT NULL,"
        "asof_open_time_ms INTEGER NOT NULL,"
        "generated_at_ms INTEGER NOT NULL,"
        "horizon_bars INTEGER NOT NULL,"
        "score REAL NOT NULL,"
        "score_percentile REAL,"
        "direction TEXT NOT NULL CHECK (direction IN ('long','short','none')),"
        "confidence REAL NOT NULL,"
        "execution_mode TEXT NOT NULL,"
        "blocked_stage TEXT,"
        "would_place_order INTEGER NOT NULL,"
        "current_price REAL,"
        "atr REAL,"
        "reason TEXT,"
        "captured_at_ms INTEGER NOT NULL"
        ");");

    execOrThrow(
        m_db,
        "CREATE TABLE IF NOT EXISTS qlib_shadow_outcomes ("
        "shadow_id TEXT PRIMARY KEY,"
        "raw_return REAL NOT NULL,"
        "direction_return REAL NOT NULL,"
        "net_return REAL NOT NULL,"
        "hit INTEGER NOT NULL,"
        "matured_at_ms INTEGER NOT NULL,"
        "FOREIGN KEY (shadow_id) REFERENCES qlib_shadow_signals(shadow_id)"
        ");");
    execOrThrow(
        m_db,
        "CREATE INDEX IF NOT EXISTS idx_shadow_signals_asof "
        "ON qlib_shadow_signals (interval, asof_open_time_ms);");

    execOrThrow(
        m_db,
        "CREATE TABLE IF NOT EXISTS qlib_predictions ("
        "model_id TEXT NOT NULL,"
        "run_id TEXT,"
        "symbol TEXT NOT NULL,"
        "interval TEXT NOT NULL,"
        "asof_open_time_ms INTEGER NOT NULL,"
        "generated_at_ms INTEGER NOT NULL,"
        "horizon_bars INTEGER NOT NULL,"
        "score REAL NOT NULL,"
        "rank INTEGER,"
        "score_percentile REAL,"
        "PRIMARY KEY (model_id, symbol, interval, asof_open_time_ms)"
        ");");

    auto hasColumn = [this](std::string_view table, std::string_view column) {
        std::string sql = "PRAGMA table_info(" + std::string(table) + ");";
        sqlite3_stmt* rawStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
            return false;
        }
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const std::string name = columnText(stmt.get(), 1);
            if (name == column) {
                return true;
            }
        }
        return false;
    };
    if (!hasColumn("qlib_predictions", "run_id")) {
        execOrThrow(m_db, "ALTER TABLE qlib_predictions ADD COLUMN run_id TEXT;");
    }
    if (!hasColumn("qlib_predictions", "horizon_bars")) {
        execOrThrow(m_db, "ALTER TABLE qlib_predictions ADD COLUMN horizon_bars INTEGER NOT NULL DEFAULT 1;");
    }
}

void ShadowMetricsRecorder::recordShadowSignal(const ShadowSignalRecord& record) {
    std::lock_guard<std::mutex> lock(m_mutex);
    int64_t lookupAsofOpenMs = record.asofOpenTimeMs;
    if (lookupAsofOpenMs <= 0 && record.capturedAtMs > 0) {
        const int64_t intervalMs = intervalToMs(record.interval);
        if (intervalMs > 0) {
            lookupAsofOpenMs = (record.capturedAtMs / intervalMs) * intervalMs;
        }
    }

    sqlite3_stmt* predStmt = predictionLookupStmtLocked();
    sqlite3_reset(predStmt);
    sqlite3_clear_bindings(predStmt);
    bindText(predStmt, 1, record.modelId);
    bindText(predStmt, 2, record.symbol);
    bindText(predStmt, 3, record.interval);
    sqlite3_bind_int64(predStmt, 4, lookupAsofOpenMs);
    sqlite3_bind_int64(predStmt, 5, lookupAsofOpenMs);

    std::string runId = record.runId;
    int64_t asofOpenMs = 0;
    int64_t generatedAtMs = 0;
    int horizonBars = m_config.horizonBars;
    double score = 0.0;
    double scorePercentile = 0.0;
    bool hasScorePercentile = false;

    const int predRc = sqlite3_step(predStmt);
    if (predRc == SQLITE_ROW) {
        if (runId.empty()) {
            runId = columnText(predStmt, 0);
        }
        asofOpenMs = sqlite3_column_int64(predStmt, 1);
        generatedAtMs = sqlite3_column_int64(predStmt, 2);
        horizonBars = sqlite3_column_int(predStmt, 3);
        score = sqlite3_column_double(predStmt, 4);
        if (sqlite3_column_type(predStmt, 5) != SQLITE_NULL) {
            hasScorePercentile = true;
            scorePercentile = sqlite3_column_double(predStmt, 5);
        }
    } else if (predRc != SQLITE_DONE) {
        sqlite3_reset(predStmt);
        throw std::runtime_error("ShadowMetricsRecorder prediction lookup failed");
    }
    sqlite3_reset(predStmt);
    sqlite3_clear_bindings(predStmt);
    if (generatedAtMs <= 0) {
        generatedAtMs = record.capturedAtMs;
    }
    if (asofOpenMs <= 0) {
        if (lookupAsofOpenMs > 0) {
            asofOpenMs = lookupAsofOpenMs;
        } else {
            asofOpenMs = record.capturedAtMs;
        }
    }
    if (horizonBars <= 0) {
        horizonBars = m_config.horizonBars;
    }

    const std::string shadowId = makeShadowId(
        record.modelId.empty() ? m_config.modelId : record.modelId,
        record.symbol,
        record.interval,
        asofOpenMs,
        record.capturedAtMs);

    sqlite3_stmt* insertStmt = insertShadowSignalStmtLocked();
    sqlite3_reset(insertStmt);
    sqlite3_clear_bindings(insertStmt);
    bindText(insertStmt, 1, shadowId);
    bindText(insertStmt, 2, record.modelId.empty() ? m_config.modelId : record.modelId);
    bindText(insertStmt, 3, runId);
    bindText(insertStmt, 4, record.symbol);
    bindText(insertStmt, 5, record.interval);
    sqlite3_bind_int64(insertStmt, 6, asofOpenMs);
    sqlite3_bind_int64(insertStmt, 7, generatedAtMs);
    sqlite3_bind_int(insertStmt, 8, horizonBars);
    sqlite3_bind_double(insertStmt, 9, score);
    if (hasScorePercentile) {
        sqlite3_bind_double(insertStmt, 10, scorePercentile);
    } else {
        sqlite3_bind_null(insertStmt, 10);
    }
    bindText(insertStmt, 11, directionToDb(record.direction));
    sqlite3_bind_double(insertStmt, 12, record.confidence);
    bindText(insertStmt, 13, modeToDb(record.executionMode));
    if (record.blockedStage.empty()) {
        sqlite3_bind_null(insertStmt, 14);
    } else {
        bindText(insertStmt, 14, record.blockedStage);
    }
    sqlite3_bind_int(insertStmt, 15, record.wouldPlaceOrder ? 1 : 0);
    if (record.currentPrice > 0.0) {
        sqlite3_bind_double(insertStmt, 16, record.currentPrice);
    } else {
        sqlite3_bind_null(insertStmt, 16);
    }
    if (record.atr > 0.0) {
        sqlite3_bind_double(insertStmt, 17, record.atr);
    } else {
        sqlite3_bind_null(insertStmt, 17);
    }
    bindText(insertStmt, 18, record.reason);
    sqlite3_bind_int64(insertStmt, 19, record.capturedAtMs);
    if (sqlite3_step(insertStmt) != SQLITE_DONE) {
        sqlite3_reset(insertStmt);
        throw std::runtime_error("ShadowMetricsRecorder insert shadow signal failed");
    }
    sqlite3_reset(insertStmt);
    sqlite3_clear_bindings(insertStmt);
}

void ShadowMetricsRecorder::onCandleClosed(std::string_view symbol, std::string_view interval, const Kline& kline) {
    std::lock_guard<std::mutex> lock(m_mutex);
    upsertCandleLocked(symbol, interval, kline);
    upsertActualReturnsLocked(interval);
    upsertShadowOutcomesLocked(interval);
}

std::string ShadowMetricsRecorder::directionToDb(strategy::Signal::Direction direction) {
    switch (direction) {
        case strategy::Signal::Direction::Long:
            return "long";
        case strategy::Signal::Direction::Short:
            return "short";
        case strategy::Signal::Direction::None:
            return "none";
    }
    return "none";
}

std::string ShadowMetricsRecorder::modeToDb(ExecutionMode mode) {
    switch (mode) {
        case ExecutionMode::Disabled:
            return "disabled";
        case ExecutionMode::Shadow:
            return "shadow";
        case ExecutionMode::LiveCanary:
            return "live_canary";
        case ExecutionMode::Live:
            return "live";
    }
    return "shadow";
}

int64_t ShadowMetricsRecorder::intervalToMs(const std::string& interval) {
    if (interval.size() < 2) {
        return 0;
    }
    const char suffix = interval.back();
    int value = 0;
    const std::string numberText = interval.substr(0, interval.size() - 1);
    const auto* begin = numberText.data();
    const auto* end = numberText.data() + numberText.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value <= 0) {
        return 0;
    }
    if (suffix == 'm') {
        return static_cast<int64_t>(value) * 60LL * 1000LL;
    }
    if (suffix == 'h') {
        return static_cast<int64_t>(value) * 60LL * 60LL * 1000LL;
    }
    if (suffix == 'd') {
        return static_cast<int64_t>(value) * 24LL * 60LL * 60LL * 1000LL;
    }
    return 0;
}

sqlite3_stmt* ShadowMetricsRecorder::predictionLookupStmtLocked() {
    if (m_predictionLookupStmt) {
        return m_predictionLookupStmt;
    }
    const char* sql =
        "SELECT run_id, asof_open_time_ms, generated_at_ms, horizon_bars, score, score_percentile "
        "FROM qlib_predictions "
        "WHERE model_id = ? AND symbol = ? AND interval = ? "
        "AND (? <= 0 OR asof_open_time_ms = ?) "
        "ORDER BY generated_at_ms DESC LIMIT 1;";
    if (sqlite3_prepare_v2(m_db, sql, -1, &m_predictionLookupStmt, nullptr) != SQLITE_OK ||
        m_predictionLookupStmt == nullptr) {
        throw std::runtime_error("ShadowMetricsRecorder prepare prediction lookup failed");
    }
    return m_predictionLookupStmt;
}

sqlite3_stmt* ShadowMetricsRecorder::insertShadowSignalStmtLocked() {
    if (m_insertShadowSignalStmt) {
        return m_insertShadowSignalStmt;
    }
    const char* sql =
        "INSERT OR REPLACE INTO qlib_shadow_signals("
        "shadow_id, model_id, run_id, symbol, interval, asof_open_time_ms, generated_at_ms, horizon_bars,"
        "score, score_percentile, direction, confidence, execution_mode, blocked_stage, would_place_order,"
        "current_price, atr, reason, captured_at_ms"
        ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(m_db, sql, -1, &m_insertShadowSignalStmt, nullptr) != SQLITE_OK ||
        m_insertShadowSignalStmt == nullptr) {
        throw std::runtime_error("ShadowMetricsRecorder prepare insert shadow signal failed");
    }
    return m_insertShadowSignalStmt;
}

void ShadowMetricsRecorder::finalizeStatements() noexcept {
    if (m_predictionLookupStmt) {
        sqlite3_finalize(m_predictionLookupStmt);
        m_predictionLookupStmt = nullptr;
    }
    if (m_insertShadowSignalStmt) {
        sqlite3_finalize(m_insertShadowSignalStmt);
        m_insertShadowSignalStmt = nullptr;
    }
}

void ShadowMetricsRecorder::upsertCandleLocked(std::string_view symbol, std::string_view interval, const Kline& kline) {
    const char* sql =
        "INSERT INTO qlib_candles("
        "symbol, interval, open_time_ms, close_time_ms, open, high, low, close, volume, quote_volume, trade_count, inserted_at_ms"
        ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(symbol, interval, open_time_ms) DO UPDATE SET "
        "close_time_ms=excluded.close_time_ms,"
        "open=excluded.open,"
        "high=excluded.high,"
        "low=excluded.low,"
        "close=excluded.close,"
        "volume=excluded.volume,"
        "quote_volume=excluded.quote_volume,"
        "trade_count=excluded.trade_count,"
        "inserted_at_ms=excluded.inserted_at_ms;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        throw std::runtime_error("ShadowMetricsRecorder prepare upsert candle failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    bindText(stmt.get(), 1, symbol);
    bindText(stmt.get(), 2, interval);
    sqlite3_bind_int64(stmt.get(), 3, kline.openTime);
    sqlite3_bind_int64(stmt.get(), 4, kline.closeTime);
    sqlite3_bind_double(stmt.get(), 5, kline.open);
    sqlite3_bind_double(stmt.get(), 6, kline.high);
    sqlite3_bind_double(stmt.get(), 7, kline.low);
    sqlite3_bind_double(stmt.get(), 8, kline.close);
    sqlite3_bind_double(stmt.get(), 9, kline.volume);
    sqlite3_bind_double(stmt.get(), 10, kline.quoteVolume);
    sqlite3_bind_int(stmt.get(), 11, kline.tradeCount);
    sqlite3_bind_int64(stmt.get(), 12, nowMs());
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error("ShadowMetricsRecorder upsert candle failed");
    }
}

void ShadowMetricsRecorder::upsertActualReturnsLocked(std::string_view interval) {
    const int64_t stepMs = intervalToMs(std::string(interval));
    if (stepMs <= 0) {
        return;
    }
    const int64_t horizon = std::max(1, m_config.horizonBars);
    const int64_t horizonMs = horizon * stepMs;

    const char* selectSql =
        "SELECT e.symbol, e.open_time_ms, e.close, x.open_time_ms, x.close "
        "FROM qlib_candles e "
        "JOIN qlib_candles x "
        "  ON x.symbol = e.symbol "
        " AND x.interval = e.interval "
        " AND x.open_time_ms = e.open_time_ms + ? "
        "LEFT JOIN qlib_actual_returns ar "
        "  ON ar.symbol = e.symbol "
        " AND ar.interval = e.interval "
        " AND ar.asof_open_time_ms = e.open_time_ms "
        " AND ar.horizon_bars = ? "
        "WHERE e.interval = ? AND ar.symbol IS NULL;";
    sqlite3_stmt* selectRaw = nullptr;
    if (sqlite3_prepare_v2(m_db, selectSql, -1, &selectRaw, nullptr) != SQLITE_OK || selectRaw == nullptr) {
        throw std::runtime_error("ShadowMetricsRecorder prepare select actual returns failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> selectStmt(selectRaw, sqlite3_finalize);
    sqlite3_bind_int64(selectStmt.get(), 1, horizonMs);
    sqlite3_bind_int(selectStmt.get(), 2, static_cast<int>(horizon));
    bindText(selectStmt.get(), 3, interval);

    const char* insertSql =
        "INSERT OR IGNORE INTO qlib_actual_returns("
        "symbol, interval, asof_open_time_ms, horizon_bars, exit_open_time_ms, entry_close, exit_close, raw_return, computed_at_ms"
        ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* insertRaw = nullptr;
    if (sqlite3_prepare_v2(m_db, insertSql, -1, &insertRaw, nullptr) != SQLITE_OK || insertRaw == nullptr) {
        throw std::runtime_error("ShadowMetricsRecorder prepare insert actual returns failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> insertStmt(insertRaw, sqlite3_finalize);

    while (sqlite3_step(selectStmt.get()) == SQLITE_ROW) {
        const std::string symbol = columnText(selectStmt.get(), 0);
        const int64_t asofOpen = sqlite3_column_int64(selectStmt.get(), 1);
        const double entryClose = sqlite3_column_double(selectStmt.get(), 2);
        const int64_t exitOpen = sqlite3_column_int64(selectStmt.get(), 3);
        const double exitClose = sqlite3_column_double(selectStmt.get(), 4);
        if (entryClose <= 0.0) {
            continue;
        }
        const double rawReturn = (exitClose / entryClose) - 1.0;

        sqlite3_reset(insertStmt.get());
        sqlite3_clear_bindings(insertStmt.get());
        bindText(insertStmt.get(), 1, symbol);
        bindText(insertStmt.get(), 2, interval);
        sqlite3_bind_int64(insertStmt.get(), 3, asofOpen);
        sqlite3_bind_int(insertStmt.get(), 4, static_cast<int>(horizon));
        sqlite3_bind_int64(insertStmt.get(), 5, exitOpen);
        sqlite3_bind_double(insertStmt.get(), 6, entryClose);
        sqlite3_bind_double(insertStmt.get(), 7, exitClose);
        sqlite3_bind_double(insertStmt.get(), 8, rawReturn);
        sqlite3_bind_int64(insertStmt.get(), 9, nowMs());
        if (sqlite3_step(insertStmt.get()) != SQLITE_DONE) {
            throw std::runtime_error("ShadowMetricsRecorder insert actual return failed");
        }
    }
}

void ShadowMetricsRecorder::upsertShadowOutcomesLocked(std::string_view interval) {
    const int64_t stepMs = intervalToMs(std::string(interval));
    if (stepMs <= 0) {
        return;
    }
    const char* selectSql =
        "SELECT s.shadow_id, s.direction, s.horizon_bars, e.close, x.close "
        "FROM qlib_shadow_signals s "
        "JOIN qlib_candles e "
        "  ON e.symbol = s.symbol "
        " AND e.interval = s.interval "
        " AND e.open_time_ms = s.asof_open_time_ms "
        "JOIN qlib_candles x "
        "  ON x.symbol = s.symbol "
        " AND x.interval = s.interval "
        " AND x.open_time_ms = s.asof_open_time_ms + (CAST(s.horizon_bars AS INTEGER) * ?) "
        "LEFT JOIN qlib_shadow_outcomes o ON o.shadow_id = s.shadow_id "
        "WHERE s.interval = ? AND o.shadow_id IS NULL AND s.would_place_order = 1;";
    sqlite3_stmt* selectRaw = nullptr;
    if (sqlite3_prepare_v2(m_db, selectSql, -1, &selectRaw, nullptr) != SQLITE_OK || selectRaw == nullptr) {
        throw std::runtime_error("ShadowMetricsRecorder prepare select shadow outcomes failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> selectStmt(selectRaw, sqlite3_finalize);
    sqlite3_bind_int64(selectStmt.get(), 1, stepMs);
    bindText(selectStmt.get(), 2, interval);

    const char* insertSql =
        "INSERT OR IGNORE INTO qlib_shadow_outcomes("
        "shadow_id, raw_return, direction_return, net_return, hit, matured_at_ms"
        ") VALUES(?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* insertRaw = nullptr;
    if (sqlite3_prepare_v2(m_db, insertSql, -1, &insertRaw, nullptr) != SQLITE_OK || insertRaw == nullptr) {
        throw std::runtime_error("ShadowMetricsRecorder prepare insert shadow outcomes failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> insertStmt(insertRaw, sqlite3_finalize);

    while (sqlite3_step(selectStmt.get()) == SQLITE_ROW) {
        const std::string shadowId = columnText(selectStmt.get(), 0);
        const std::string directionText = columnText(selectStmt.get(), 1);
        const int horizonBars = sqlite3_column_int(selectStmt.get(), 2);
        const double entryClose = sqlite3_column_double(selectStmt.get(), 3);
        const double exitClose = sqlite3_column_double(selectStmt.get(), 4);
        if (entryClose <= 0.0 || horizonBars <= 0) {
            continue;
        }
        const double rawReturn = (exitClose / entryClose) - 1.0;
        double directionReturn = 0.0;
        if (directionText == "long") {
            directionReturn = rawReturn;
        } else if (directionText == "short") {
            directionReturn = -rawReturn;
        } else {
            continue;
        }

        const double durationDays = (static_cast<double>(horizonBars) * static_cast<double>(stepMs)) /
            (24.0 * 60.0 * 60.0 * 1000.0);
        const double netReturn = directionReturn
            - (m_config.costModel.estimatedRoundTripFeeBps / 10000.0)
            - (m_config.costModel.estimatedSlippageBps / 10000.0)
            - fundingCost(durationDays);
        const int hit = directionReturn > 0.0 ? 1 : 0;

        sqlite3_reset(insertStmt.get());
        sqlite3_clear_bindings(insertStmt.get());
        bindText(insertStmt.get(), 1, shadowId);
        sqlite3_bind_double(insertStmt.get(), 2, rawReturn);
        sqlite3_bind_double(insertStmt.get(), 3, directionReturn);
        sqlite3_bind_double(insertStmt.get(), 4, netReturn);
        sqlite3_bind_int(insertStmt.get(), 5, hit);
        sqlite3_bind_int64(insertStmt.get(), 6, nowMs());
        if (sqlite3_step(insertStmt.get()) != SQLITE_DONE) {
            throw std::runtime_error("ShadowMetricsRecorder insert shadow outcome failed");
        }
    }
}

int64_t ShadowMetricsRecorder::nowMs() const {
    return sqlite_helpers::nowMs();
}

double ShadowMetricsRecorder::fundingCost(double durationDays) const {
    if (durationDays <= 0.0 || m_config.costModel.estimatedFundingBpsPerDay <= 0.0) {
        return 0.0;
    }
    return (m_config.costModel.estimatedFundingBpsPerDay * durationDays) / 10000.0;
}

} // namespace orchestration
