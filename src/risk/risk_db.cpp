#include "risk/risk_db.h"

#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#elif __has_include(<winsqlite/winsqlite3.h>)
#include <winsqlite/winsqlite3.h>
#else
#error "SQLite headers not found"
#endif

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace engine {

namespace {

void execOrThrow(sqlite3* db, const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc == SQLITE_OK) {
        return;
    }
    const std::string message = err ? err : "sqlite error";
    if (err) {
        sqlite3_free(err);
    }
    throw std::runtime_error(message);
}

void bindText(sqlite3_stmt* stmt, int idx, std::string_view value) {
    sqlite3_bind_text(stmt, idx, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

std::string columnTextOrEmpty(sqlite3_stmt* stmt, int col) {
    const char* raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return raw ? std::string(raw) : std::string{};
}

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

Statement prepareStatement(sqlite3* db, const char* sql, const char* context) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("RiskDb prepare ") + context + " failed");
    }
    return Statement(raw, sqlite3_finalize);
}

} // namespace

RiskDb::RiskDb(const std::string& dbPath) {
    const std::filesystem::path path(dbPath);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    if (sqlite3_open(dbPath.c_str(), &m_db) != SQLITE_OK || m_db == nullptr) {
        const std::string message = m_db ? sqlite3_errmsg(m_db) : "sqlite3_open failed";
        if (m_db) {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
        throw std::runtime_error("RiskDb open failed: " + message);
    }
    execOrThrow(m_db, "PRAGMA journal_mode=WAL;");
    execOrThrow(m_db, "PRAGMA synchronous=NORMAL;");
    initSchema();
}

RiskDb::~RiskDb() {
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

void RiskDb::initSchema() {
    execOrThrow(
        m_db,
        "CREATE TABLE IF NOT EXISTS equity_points ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "timestamp_ms INTEGER NOT NULL,"
        "equity REAL NOT NULL,"
        "year INTEGER NOT NULL,"
        "source TEXT NOT NULL,"
        "basis TEXT NOT NULL"
        ");");
    execOrThrow(
        m_db,
        "CREATE INDEX IF NOT EXISTS idx_ep_year_ts ON equity_points(year, timestamp_ms);");
    execOrThrow(
        m_db,
        "CREATE INDEX IF NOT EXISTS idx_ep_basis_ts ON equity_points(basis, timestamp_ms);");

    execOrThrow(
        m_db,
        "CREATE TABLE IF NOT EXISTS risk_metrics_cache ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "computed_at_ms INTEGER NOT NULL,"
        "window_kind TEXT NOT NULL,"
        "window_start_ms INTEGER NOT NULL,"
        "window_end_ms INTEGER NOT NULL,"
        "basis TEXT NOT NULL,"
        "data_points INTEGER NOT NULL,"
        "valid INTEGER NOT NULL,"
        "annual_return REAL,"
        "excess_return REAL,"
        "std_dev_all REAL,"
        "std_dev_downside REAL,"
        "ulcer_index REAL,"
        "max_drawdown REAL,"
        "sharpe_ratio REAL,"
        "sortino_ratio REAL,"
        "upi REAL"
        ");");
    execOrThrow(
        m_db,
        "CREATE INDEX IF NOT EXISTS idx_rmc_window ON risk_metrics_cache(window_kind, basis, window_end_ms);");
}

void RiskDb::insertEquityPoint(const EquityPoint& p) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql =
        "INSERT INTO equity_points(timestamp_ms, equity, year, source, basis) VALUES(?, ?, ?, ?, ?);";
    auto stmt = prepareStatement(m_db, sql, "insertEquityPoint");
    sqlite3_bind_int64(stmt.get(), 1, p.timestampMs);
    sqlite3_bind_double(stmt.get(), 2, p.equity);
    sqlite3_bind_int(stmt.get(), 3, p.year);
    bindText(stmt.get(), 4, p.source);
    bindText(stmt.get(), 5, p.basis);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error("RiskDb step insertEquityPoint failed");
    }
}

std::vector<EquityPoint> RiskDb::getByYear(int year) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<EquityPoint> out;
    const char* sql =
        "SELECT id, timestamp_ms, equity, year, source, basis FROM equity_points "
        "WHERE year = ? ORDER BY timestamp_ms ASC, id ASC;";
    auto stmt = prepareStatement(m_db, sql, "getByYear");
    sqlite3_bind_int(stmt.get(), 1, year);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        EquityPoint p;
        p.id = sqlite3_column_int64(stmt.get(), 0);
        p.timestampMs = sqlite3_column_int64(stmt.get(), 1);
        p.equity = sqlite3_column_double(stmt.get(), 2);
        p.year = sqlite3_column_int(stmt.get(), 3);
        p.source = columnTextOrEmpty(stmt.get(), 4);
        p.basis = columnTextOrEmpty(stmt.get(), 5);
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<EquityPoint> RiskDb::getByTimeRange(std::string_view basis, int64_t startMs, int64_t endMs) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<EquityPoint> out;
    const char* sql =
        "SELECT id, timestamp_ms, equity, year, source, basis FROM equity_points "
        "WHERE basis = ? AND timestamp_ms >= ? AND timestamp_ms <= ? "
        "ORDER BY timestamp_ms ASC, id ASC;";
    auto stmt = prepareStatement(m_db, sql, "getByTimeRange");
    bindText(stmt.get(), 1, basis);
    sqlite3_bind_int64(stmt.get(), 2, startMs);
    sqlite3_bind_int64(stmt.get(), 3, endMs);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        EquityPoint p;
        p.id = sqlite3_column_int64(stmt.get(), 0);
        p.timestampMs = sqlite3_column_int64(stmt.get(), 1);
        p.equity = sqlite3_column_double(stmt.get(), 2);
        p.year = sqlite3_column_int(stmt.get(), 3);
        p.source = columnTextOrEmpty(stmt.get(), 4);
        p.basis = columnTextOrEmpty(stmt.get(), 5);
        out.push_back(std::move(p));
    }
    return out;
}

void RiskDb::insertMetrics(const RiskMetricsResult& m) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql =
        "INSERT INTO risk_metrics_cache("
        "computed_at_ms, window_kind, window_start_ms, window_end_ms, basis, "
        "data_points, valid, annual_return, excess_return, std_dev_all, std_dev_downside, "
        "ulcer_index, max_drawdown, sharpe_ratio, sortino_ratio, upi"
        ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    auto stmt = prepareStatement(m_db, sql, "insertMetrics");
    sqlite3_bind_int64(stmt.get(), 1, m.computedAtMs);
    bindText(stmt.get(), 2, m.windowKind);
    sqlite3_bind_int64(stmt.get(), 3, m.windowStartMs);
    sqlite3_bind_int64(stmt.get(), 4, m.windowEndMs);
    bindText(stmt.get(), 5, m.basis);
    sqlite3_bind_int(stmt.get(), 6, m.dataPoints);
    sqlite3_bind_int(stmt.get(), 7, m.valid ? 1 : 0);
    sqlite3_bind_double(stmt.get(), 8, m.annualReturn);
    sqlite3_bind_double(stmt.get(), 9, m.excessReturn);
    sqlite3_bind_double(stmt.get(), 10, m.stdDevAll);
    sqlite3_bind_double(stmt.get(), 11, m.stdDevDownside);
    sqlite3_bind_double(stmt.get(), 12, m.ulcerIndex);
    sqlite3_bind_double(stmt.get(), 13, m.maxDrawdown);
    sqlite3_bind_double(stmt.get(), 14, m.sharpeRatio);
    sqlite3_bind_double(stmt.get(), 15, m.sortinoRatio);
    sqlite3_bind_double(stmt.get(), 16, m.upi);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error("RiskDb step insertMetrics failed");
    }
}

std::optional<RiskMetricsResult> RiskDb::getLatestMetrics(std::string_view windowKind, std::string_view basis) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql =
        "SELECT computed_at_ms, window_kind, window_start_ms, window_end_ms, basis, "
        "data_points, valid, annual_return, excess_return, std_dev_all, std_dev_downside, "
        "ulcer_index, max_drawdown, sharpe_ratio, sortino_ratio, upi "
        "FROM risk_metrics_cache WHERE window_kind = ? AND basis = ? "
        "ORDER BY window_end_ms DESC, id DESC LIMIT 1;";
    auto stmt = prepareStatement(m_db, sql, "getLatestMetrics");
    bindText(stmt.get(), 1, windowKind);
    bindText(stmt.get(), 2, basis);
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW) {
        return std::nullopt;
    }
    RiskMetricsResult m;
    m.computedAtMs = sqlite3_column_int64(stmt.get(), 0);
    m.windowKind = columnTextOrEmpty(stmt.get(), 1);
    m.windowStartMs = sqlite3_column_int64(stmt.get(), 2);
    m.windowEndMs = sqlite3_column_int64(stmt.get(), 3);
    m.basis = columnTextOrEmpty(stmt.get(), 4);
    m.dataPoints = sqlite3_column_int(stmt.get(), 5);
    m.valid = sqlite3_column_int(stmt.get(), 6) != 0;
    m.annualReturn = sqlite3_column_double(stmt.get(), 7);
    m.excessReturn = sqlite3_column_double(stmt.get(), 8);
    m.stdDevAll = sqlite3_column_double(stmt.get(), 9);
    m.stdDevDownside = sqlite3_column_double(stmt.get(), 10);
    m.ulcerIndex = sqlite3_column_double(stmt.get(), 11);
    m.maxDrawdown = sqlite3_column_double(stmt.get(), 12);
    m.sharpeRatio = sqlite3_column_double(stmt.get(), 13);
    m.sortinoRatio = sqlite3_column_double(stmt.get(), 14);
    m.upi = sqlite3_column_double(stmt.get(), 15);
    return m;
}

void RiskDb::deleteEquityPointsOlderThan(int64_t cutoffMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql = "DELETE FROM equity_points WHERE timestamp_ms < ?;";
    auto stmt = prepareStatement(m_db, sql, "deleteEquityPointsOlderThan");
    sqlite3_bind_int64(stmt.get(), 1, cutoffMs);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error("RiskDb step deleteEquityPointsOlderThan failed");
    }
}

} // namespace engine
