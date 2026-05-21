#include "orchestration/qlib_state_store.h"

#include "logger.h"
#include "orchestration/sqlite_helpers.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace orchestration {

namespace {

using sqlite_helpers::bindText;
using sqlite_helpers::columnText;
using sqlite_helpers::execOrThrow;
using sqlite_helpers::nowMs;

} // namespace

std::shared_ptr<QlibStateStore> QlibStateStore::create(QlibStateStoreConfig config) {
    return std::make_shared<QlibStateStore>(std::move(config));
}

QlibStateStore::QlibStateStore(QlibStateStoreConfig config)
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
        throw std::runtime_error("QlibStateStore open failed: " + message);
    }
    execOrThrow(m_db, "PRAGMA journal_mode=WAL;");
    execOrThrow(m_db, "PRAGMA synchronous=NORMAL;");
    execOrThrow(m_db, "PRAGMA busy_timeout=5000;");
}

QlibStateStore::~QlibStateStore() {
    stopReloadLoop();
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

void QlibStateStore::initializeSchema() {
    std::lock_guard<std::mutex> lock(m_mutex);
    execOrThrow(
        m_db,
        "CREATE TABLE IF NOT EXISTS qlib_runtime_state ("
        "model_id TEXT NOT NULL,"
        "interval TEXT NOT NULL,"
        "execution_mode TEXT NOT NULL CHECK (execution_mode IN ('disabled','shadow','live_canary','live')),"
        "active_run_id TEXT,"
        "active_manifest_path TEXT,"
        "state_version INTEGER NOT NULL DEFAULT 0,"
        "promoted_at_ms INTEGER,"
        "rollback_reason TEXT,"
        "updated_at_ms INTEGER NOT NULL,"
        "PRIMARY KEY (model_id, interval)"
        ");");

    execOrThrow(
        m_db,
        "CREATE TABLE IF NOT EXISTS qlib_model_runs ("
        "run_id TEXT PRIMARY KEY,"
        "model_id TEXT NOT NULL,"
        "interval TEXT NOT NULL,"
        "horizon_bars INTEGER NOT NULL,"
        "model_path TEXT NOT NULL,"
        "manifest_path TEXT NOT NULL,"
        "report_path TEXT NOT NULL,"
        "feature_schema_hash TEXT NOT NULL,"
        "dataset_fingerprint TEXT NOT NULL,"
        "oos_ic REAL,"
        "oos_rank_ic REAL,"
        "oos_rows INTEGER,"
        "trained_at_ms INTEGER NOT NULL,"
        "published_at_ms INTEGER,"
        "status TEXT NOT NULL CHECK (status IN ('staged','active','rejected','retired'))"
        ");");

    execOrThrow(
        m_db,
        "CREATE TABLE IF NOT EXISTS qlib_job_runs ("
        "job_id TEXT PRIMARY KEY,"
        "job_type TEXT NOT NULL,"
        "schedule_key TEXT NOT NULL,"
        "status TEXT NOT NULL CHECK (status IN ('running','succeeded','failed','stale')),"
        "pid INTEGER,"
        "started_at_ms INTEGER NOT NULL,"
        "completed_at_ms INTEGER,"
        "exit_code INTEGER,"
        "log_path TEXT,"
        "error TEXT"
        ");");
}

void QlibStateStore::initializeRuntimeStateIfMissing() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql =
        "INSERT INTO qlib_runtime_state("
        "model_id, interval, execution_mode, active_run_id, active_manifest_path, state_version, promoted_at_ms, rollback_reason, updated_at_ms"
        ") VALUES(?, ?, 'shadow', NULL, NULL, 0, NULL, NULL, ?) "
        "ON CONFLICT(model_id, interval) DO NOTHING;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        throw std::runtime_error("QlibStateStore prepare initializeRuntimeStateIfMissing failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    bindText(stmt.get(), 1, m_config.modelId);
    bindText(stmt.get(), 2, m_config.interval);
    sqlite3_bind_int64(stmt.get(), 3, nowMs());
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error("QlibStateStore step initializeRuntimeStateIfMissing failed");
    }
    m_snapshot = loadSnapshotLocked();
}

void QlibStateStore::startReloadLoop(boost::asio::io_context& ioc) {
    if (m_reloadRunning.exchange(true)) {
        return;
    }
    m_reloadTimer = std::make_unique<boost::asio::steady_timer>(ioc);
    reloadStateOnce();
    scheduleReload();
}

void QlibStateStore::stopReloadLoop() {
    m_reloadRunning.store(false);
    if (!m_reloadTimer) {
        return;
    }
    boost::system::error_code ec;
    m_reloadTimer->cancel(ec);
}

bool QlibStateStore::setExecutionMode(ExecutionMode mode, std::string rollbackReason) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql =
        "UPDATE qlib_runtime_state "
        "SET execution_mode = ?, rollback_reason = ?, state_version = state_version + 1, updated_at_ms = ? "
        "WHERE model_id = ? AND interval = ?;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        throw std::runtime_error("QlibStateStore prepare setExecutionMode failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    bindText(stmt.get(), 1, modeToDb(mode));
    bindText(stmt.get(), 2, rollbackReason);
    sqlite3_bind_int64(stmt.get(), 3, nowMs());
    bindText(stmt.get(), 4, m_config.modelId);
    bindText(stmt.get(), 5, m_config.interval);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error("QlibStateStore step setExecutionMode failed");
    }
    if (sqlite3_changes(m_db) <= 0) {
        return false;
    }
    m_snapshot = loadSnapshotLocked();
    return true;
}

RuntimeStateSnapshot QlibStateStore::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

void QlibStateStore::reloadStateOnce() {
    std::lock_guard<std::mutex> lock(m_mutex);
    try {
        m_snapshot = loadSnapshotLocked();
    } catch (const std::exception& e) {
        RuntimeStateSnapshot fallback;
        fallback.available = false;
        fallback.mode = ExecutionMode::Disabled;
        fallback.modelId = m_config.modelId;
        fallback.interval = m_config.interval;
        m_snapshot = std::move(fallback);
        Logger::instance().log(
            LogLevel::Warning,
            std::string("QlibStateStore reload failed, qlib execution disabled: ") + e.what());
    }
}

void QlibStateStore::scheduleReload() {
    if (!m_reloadTimer || !m_reloadRunning.load()) {
        return;
    }
    m_reloadTimer->expires_after(m_config.reloadInterval);
    const std::weak_ptr<QlibStateStore> weakSelf = weak_from_this();
    m_reloadTimer->async_wait([weakSelf](const boost::system::error_code& ec) {
        const auto self = weakSelf.lock();
        if (!self || ec || !self->m_reloadRunning.load()) {
            return;
        }
        self->reloadStateOnce();
        self->scheduleReload();
    });
}

RuntimeStateSnapshot QlibStateStore::loadSnapshotLocked() const {
    const char* sql =
        "SELECT execution_mode, active_run_id, state_version "
        "FROM qlib_runtime_state WHERE model_id = ? AND interval = ?;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        throw std::runtime_error("QlibStateStore prepare loadSnapshotLocked failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    bindText(stmt.get(), 1, m_config.modelId);
    bindText(stmt.get(), 2, m_config.interval);
    const int rc = sqlite3_step(stmt.get());
    RuntimeStateSnapshot out;
    out.modelId = m_config.modelId;
    out.interval = m_config.interval;
    if (rc == SQLITE_ROW) {
        out.available = true;
        out.mode = modeFromDb(columnText(stmt.get(), 0));
        out.activeRunId = columnText(stmt.get(), 1);
        out.stateVersion = sqlite3_column_int(stmt.get(), 2);
        return out;
    }
    out.available = false;
    out.mode = ExecutionMode::Disabled;
    return out;
}

std::string QlibStateStore::modeToDb(ExecutionMode mode) {
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

ExecutionMode QlibStateStore::modeFromDb(const std::string& modeText) {
    if (modeText == "disabled") {
        return ExecutionMode::Disabled;
    }
    if (modeText == "live_canary") {
        return ExecutionMode::LiveCanary;
    }
    if (modeText == "live") {
        return ExecutionMode::Live;
    }
    return ExecutionMode::Shadow;
}

} // namespace orchestration
