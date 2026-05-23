#include "orchestration/qlib_state_store.h"

#include "logger.h"
#include "orchestration/sqlite_helpers.h"

#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace orchestration {

namespace {

using sqlite_helpers::bindText;
using sqlite_helpers::columnText;
using sqlite_helpers::execOrThrow;
using sqlite_helpers::nowMs;

bool hasColumn(sqlite3* db, std::string_view table, std::string_view column) {
    const std::string sql = "PRAGMA table_info(" + std::string(table) + ");";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        return false;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        if (columnText(stmt.get(), 1) == column) {
            return true;
        }
    }
    return false;
}

void ensureColumn(sqlite3* db, std::string_view table, std::string_view column, std::string_view definition) {
    if (hasColumn(db, table, column)) {
        return;
    }
    const std::string sql = "ALTER TABLE " + std::string(table) + " ADD COLUMN " +
        std::string(column) + " " + std::string(definition) + ";";
    execOrThrow(db, sql.c_str());
}

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

    execOrThrow(
        m_db,
        "CREATE TABLE IF NOT EXISTS qlib_adapter_runtime_state ("
        "adapter_id TEXT NOT NULL,"
        "interval TEXT NOT NULL,"
        "execution_mode TEXT NOT NULL CHECK (execution_mode IN ('disabled','shadow','shadow_only','live_canary','live')),"
        "promotion_profile TEXT NOT NULL DEFAULT 'default',"
        "active_run_id TEXT,"
        "state_version INTEGER NOT NULL DEFAULT 0,"
        "promoted_at_ms INTEGER,"
        "promoted_by TEXT,"
        "last_decision_at_ms INTEGER,"
        "last_failure_at_ms INTEGER,"
        "last_failure_reason TEXT,"
        "updated_at_ms INTEGER NOT NULL,"
        "rollback_reason TEXT,"
        "PRIMARY KEY (adapter_id, interval)"
        ");");
    ensureColumn(m_db, "qlib_adapter_runtime_state", "promotion_profile", "TEXT NOT NULL DEFAULT 'default'");
    ensureColumn(m_db, "qlib_adapter_runtime_state", "promoted_at_ms", "INTEGER");
    ensureColumn(m_db, "qlib_adapter_runtime_state", "promoted_by", "TEXT");
    ensureColumn(m_db, "qlib_adapter_runtime_state", "last_decision_at_ms", "INTEGER");
    ensureColumn(m_db, "qlib_adapter_runtime_state", "last_failure_at_ms", "INTEGER");
    ensureColumn(m_db, "qlib_adapter_runtime_state", "last_failure_reason", "TEXT");

    execOrThrow(
        m_db,
        "CREATE TABLE IF NOT EXISTS qlib_promotion_profiles ("
        "profile_name TEXT PRIMARY KEY,"
        "qlib_class TEXT NOT NULL,"
        "profile_json TEXT NOT NULL,"
        "updated_at_ms INTEGER NOT NULL"
        ");");

    execOrThrow(
        m_db,
        "CREATE TABLE IF NOT EXISTS qlib_promotion_evaluations ("
        "eval_id TEXT PRIMARY KEY,"
        "model_id TEXT NOT NULL,"
        "profile_name TEXT,"
        "interval TEXT NOT NULL,"
        "evaluated_at_ms INTEGER NOT NULL,"
        "execution_mode TEXT NOT NULL,"
        "mature_signals INTEGER NOT NULL,"
        "candles INTEGER NOT NULL,"
        "hit_rate REAL,"
        "sharpe REAL,"
        "mean_net_return_bps REAL,"
        "drift_z REAL,"
        "stale_ratio REAL,"
        "decision TEXT NOT NULL,"
        "reason TEXT"
        ");");
    ensureColumn(m_db, "qlib_promotion_evaluations", "profile_name", "TEXT");
}

void QlibStateStore::initializeRuntimeStateIfMissing(ExecutionMode defaultMode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql =
        "INSERT INTO qlib_runtime_state("
        "model_id, interval, execution_mode, active_run_id, active_manifest_path, state_version, promoted_at_ms, rollback_reason, updated_at_ms"
        ") VALUES(?, ?, ?, NULL, NULL, 0, NULL, NULL, ?) "
        "ON CONFLICT(model_id, interval) DO NOTHING;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        throw std::runtime_error("QlibStateStore prepare initializeRuntimeStateIfMissing failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    bindText(stmt.get(), 1, m_config.modelId);
    bindText(stmt.get(), 2, m_config.interval);
    bindText(stmt.get(), 3, modeToDb(defaultMode));
    sqlite3_bind_int64(stmt.get(), 4, nowMs());
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error("QlibStateStore step initializeRuntimeStateIfMissing failed");
    }
    ensureAdapterRuntimeStateLocked(defaultMode);
    m_snapshot = loadSnapshotLocked();
}

void QlibStateStore::initializeAdapterRuntimeStatesIfMissing(const std::vector<AdapterRuntimeStateSeed>& seeds) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql =
        "INSERT INTO qlib_adapter_runtime_state("
        "adapter_id, interval, execution_mode, promotion_profile, active_run_id, state_version, updated_at_ms"
        ") VALUES(?, ?, ?, ?, NULL, 0, ?) "
        "ON CONFLICT(adapter_id, interval) DO NOTHING;";
    for (const auto& seed : seeds) {
        if (seed.adapterId.empty() || seed.interval.empty()) {
            continue;
        }
        sqlite3_stmt* rawStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
            throw std::runtime_error("QlibStateStore prepare initializeAdapterRuntimeStatesIfMissing failed");
        }
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
        bindText(stmt.get(), 1, seed.adapterId);
        bindText(stmt.get(), 2, seed.interval);
        bindText(stmt.get(), 3, modeToDb(seed.executionMode));
        bindText(stmt.get(), 4, seed.promotionProfile.empty() ? "default" : seed.promotionProfile);
        sqlite3_bind_int64(stmt.get(), 5, nowMs());
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            throw std::runtime_error("QlibStateStore step initializeAdapterRuntimeStatesIfMissing failed");
        }
    }
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

bool QlibStateStore::setExecutionMode(ExecutionMode mode, std::string rollbackReason, std::string promotedBy) {
    std::lock_guard<std::mutex> lock(m_mutex);
    execOrThrow(m_db, "BEGIN IMMEDIATE TRANSACTION;");

    auto rollback = [&]() {
        try {
            execOrThrow(m_db, "ROLLBACK;");
        } catch (...) {
        }
    };

    const int64_t updatedAt = nowMs();
    const std::string modeText = modeToDb(mode);
    const bool promoted = mode == ExecutionMode::LiveCanary || mode == ExecutionMode::Live;

    const char* sql =
        "UPDATE qlib_runtime_state "
        "SET execution_mode = ?, rollback_reason = ?, state_version = state_version + 1, updated_at_ms = ? "
        "WHERE model_id = ? AND interval = ?;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        rollback();
        throw std::runtime_error("QlibStateStore prepare setExecutionMode failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    bindText(stmt.get(), 1, modeText);
    bindText(stmt.get(), 2, rollbackReason);
    sqlite3_bind_int64(stmt.get(), 3, updatedAt);
    bindText(stmt.get(), 4, m_config.modelId);
    bindText(stmt.get(), 5, m_config.interval);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        rollback();
        throw std::runtime_error("QlibStateStore step setExecutionMode failed");
    }
    if (sqlite3_changes(m_db) <= 0) {
        rollback();
        return false;
    }
    stmt.reset();

    ensureAdapterRuntimeStateLocked(mode);
    const char* adapterSql =
        "UPDATE qlib_adapter_runtime_state "
        "SET execution_mode = ?,"
        "    state_version = state_version + 1,"
        "    promoted_at_ms = CASE WHEN ? THEN ? ELSE promoted_at_ms END,"
        "    promoted_by = CASE WHEN ? THEN ? ELSE promoted_by END,"
        "    updated_at_ms = ?,"
        "    rollback_reason = ? "
        "WHERE adapter_id = ? AND interval = ?;";
    sqlite3_stmt* rawAdapterStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, adapterSql, -1, &rawAdapterStmt, nullptr) != SQLITE_OK ||
        rawAdapterStmt == nullptr) {
        rollback();
        throw std::runtime_error("QlibStateStore prepare adapter setExecutionMode failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> adapterStmt(rawAdapterStmt, sqlite3_finalize);
    bindText(adapterStmt.get(), 1, modeText);
    sqlite3_bind_int(adapterStmt.get(), 2, promoted ? 1 : 0);
    if (promoted) {
        sqlite3_bind_int64(adapterStmt.get(), 3, updatedAt);
    } else {
        sqlite3_bind_null(adapterStmt.get(), 3);
    }
    sqlite3_bind_int(adapterStmt.get(), 4, promoted ? 1 : 0);
    if (promoted && !promotedBy.empty()) {
        bindText(adapterStmt.get(), 5, promotedBy);
    } else if (promoted) {
        bindText(adapterStmt.get(), 5, "promotion_checker");
    } else {
        sqlite3_bind_null(adapterStmt.get(), 5);
    }
    sqlite3_bind_int64(adapterStmt.get(), 6, updatedAt);
    bindText(adapterStmt.get(), 7, rollbackReason);
    bindText(adapterStmt.get(), 8, m_config.modelId);
    bindText(adapterStmt.get(), 9, m_config.interval);
    if (sqlite3_step(adapterStmt.get()) != SQLITE_DONE) {
        rollback();
        throw std::runtime_error("QlibStateStore step adapter setExecutionMode failed");
    }
    if (sqlite3_changes(m_db) <= 0) {
        rollback();
        return false;
    }
    adapterStmt.reset();

    try {
        execOrThrow(m_db, "COMMIT;");
    } catch (...) {
        rollback();
        throw;
    }
    m_snapshot = loadSnapshotLocked();
    return true;
}

std::string QlibStateStore::promotionProfileName() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return promotionProfileNameLocked();
}

std::optional<std::string> QlibStateStore::promotionProfileJson(std::string_view profileName) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql = "SELECT profile_json FROM qlib_promotion_profiles WHERE profile_name = ?;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        throw std::runtime_error("QlibStateStore prepare promotionProfileJson failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    bindText(stmt.get(), 1, profileName);
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        return columnText(stmt.get(), 0);
    }
    if (rc == SQLITE_DONE) {
        return std::nullopt;
    }
    throw std::runtime_error("QlibStateStore step promotionProfileJson failed");
}

PromotionProfileSnapshot QlibStateStore::promotionProfileNameAndJson() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql =
        "SELECT s.promotion_profile, p.profile_json "
        "FROM qlib_adapter_runtime_state s "
        "LEFT JOIN qlib_promotion_profiles p ON p.profile_name = s.promotion_profile "
        "WHERE s.adapter_id = ? AND s.interval = ?;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        throw std::runtime_error("QlibStateStore prepare promotionProfileNameAndJson failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    bindText(stmt.get(), 1, m_config.modelId);
    bindText(stmt.get(), 2, m_config.interval);
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        return {};
    }
    if (rc != SQLITE_ROW) {
        throw std::runtime_error("QlibStateStore step promotionProfileNameAndJson failed");
    }

    PromotionProfileSnapshot out;
    out.profileName = columnText(stmt.get(), 0);
    if (out.profileName.empty()) {
        out.profileName = "default";
    }
    if (sqlite3_column_type(stmt.get(), 1) != SQLITE_NULL) {
        out.profileJson = columnText(stmt.get(), 1);
    }
    return out;
}

void QlibStateStore::recordPromotionEvaluation(const PromotionEvaluationRecord& record) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const int64_t evaluatedAt = nowMs();
    const auto steadySuffix = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream id;
    id << m_config.modelId << '|' << m_config.interval << '|' << evaluatedAt << '|' << steadySuffix;

    const char* sql =
        "INSERT INTO qlib_promotion_evaluations("
        "eval_id, model_id, profile_name, interval, evaluated_at_ms, execution_mode, mature_signals, candles,"
        "hit_rate, sharpe, mean_net_return_bps, drift_z, stale_ratio, decision, reason"
        ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, NULL, ?, ?);";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        throw std::runtime_error("QlibStateStore prepare recordPromotionEvaluation failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    bindText(stmt.get(), 1, id.str());
    bindText(stmt.get(), 2, m_config.modelId);
    bindText(stmt.get(), 3, record.profileName);
    bindText(stmt.get(), 4, m_config.interval);
    sqlite3_bind_int64(stmt.get(), 5, evaluatedAt);
    bindText(stmt.get(), 6, record.executionMode);
    sqlite3_bind_int(stmt.get(), 7, record.matureSignals);
    sqlite3_bind_int(stmt.get(), 8, record.candles);
    sqlite3_bind_double(stmt.get(), 9, record.hitRate);
    sqlite3_bind_double(stmt.get(), 10, record.sharpe);
    sqlite3_bind_double(stmt.get(), 11, record.meanNetReturnBps);
    bindText(stmt.get(), 12, record.decision);
    bindText(stmt.get(), 13, record.reason);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error("QlibStateStore step recordPromotionEvaluation failed");
    }

    const bool failure = record.decision != "promoted_canary" &&
        record.decision != "promoted_live" &&
        record.decision != "already_live";
    ensureAdapterRuntimeStateLocked();
    const char* updateSql =
        "UPDATE qlib_adapter_runtime_state "
        "SET last_decision_at_ms = ?,"
        "    last_failure_at_ms = CASE WHEN ? THEN ? ELSE last_failure_at_ms END,"
        "    last_failure_reason = CASE WHEN ? THEN ? ELSE last_failure_reason END,"
        "    updated_at_ms = ? "
        "WHERE adapter_id = ? AND interval = ?;";
    sqlite3_stmt* rawUpdate = nullptr;
    if (sqlite3_prepare_v2(m_db, updateSql, -1, &rawUpdate, nullptr) != SQLITE_OK || rawUpdate == nullptr) {
        throw std::runtime_error("QlibStateStore prepare promotion decision audit failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> updateStmt(rawUpdate, sqlite3_finalize);
    sqlite3_bind_int64(updateStmt.get(), 1, evaluatedAt);
    sqlite3_bind_int(updateStmt.get(), 2, failure ? 1 : 0);
    sqlite3_bind_int64(updateStmt.get(), 3, evaluatedAt);
    sqlite3_bind_int(updateStmt.get(), 4, failure ? 1 : 0);
    bindText(updateStmt.get(), 5, record.reason);
    sqlite3_bind_int64(updateStmt.get(), 6, evaluatedAt);
    bindText(updateStmt.get(), 7, m_config.modelId);
    bindText(updateStmt.get(), 8, m_config.interval);
    if (sqlite3_step(updateStmt.get()) != SQLITE_DONE) {
        throw std::runtime_error("QlibStateStore step promotion decision audit failed");
    }
}

RuntimeStateSnapshot QlibStateStore::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

RuntimeStateSnapshot QlibStateStore::snapshotForAdapter(std::string_view adapterId, std::string_view interval) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    try {
        return loadAdapterSnapshotLocked(adapterId, interval);
    } catch (const std::exception& e) {
        RuntimeStateSnapshot fallback;
        fallback.available = false;
        fallback.mode = ExecutionMode::Disabled;
        fallback.modelId = m_config.modelId;
        fallback.interval = std::string(interval);
        Logger::instance().log(
            LogLevel::Warning,
            "QlibStateStore adapter snapshot failed, adapter disabled: adapter_id=" +
                std::string(adapterId) + " interval=" + std::string(interval) + " reason=" + e.what());
        return fallback;
    }
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

RuntimeStateSnapshot QlibStateStore::loadAdapterSnapshotLocked(std::string_view adapterId, std::string_view interval) const {
    const char* sql =
        "SELECT execution_mode, active_run_id, state_version "
        "FROM qlib_adapter_runtime_state WHERE adapter_id = ? AND interval = ?;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        throw std::runtime_error("QlibStateStore prepare loadAdapterSnapshotLocked failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    bindText(stmt.get(), 1, adapterId);
    bindText(stmt.get(), 2, interval);
    const int rc = sqlite3_step(stmt.get());
    RuntimeStateSnapshot out;
    out.modelId = m_config.modelId;
    out.interval = std::string(interval);
    if (rc == SQLITE_ROW) {
        out.available = true;
        out.mode = modeFromDb(columnText(stmt.get(), 0));
        out.activeRunId = columnText(stmt.get(), 1);
        out.stateVersion = sqlite3_column_int(stmt.get(), 2);
        return out;
    }
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("QlibStateStore step loadAdapterSnapshotLocked failed");
    }
    out.available = false;
    out.mode = ExecutionMode::Disabled;
    return out;
}

void QlibStateStore::ensureAdapterRuntimeStateLocked(ExecutionMode defaultMode) {
    const char* sql =
        "INSERT INTO qlib_adapter_runtime_state("
        "adapter_id, interval, execution_mode, promotion_profile, active_run_id, state_version, updated_at_ms"
        ") VALUES(?, ?, ?, 'default', NULL, 0, ?) "
        "ON CONFLICT(adapter_id, interval) DO NOTHING;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        throw std::runtime_error("QlibStateStore prepare ensureAdapterRuntimeStateLocked failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    bindText(stmt.get(), 1, m_config.modelId);
    bindText(stmt.get(), 2, m_config.interval);
    bindText(stmt.get(), 3, modeToDb(defaultMode));
    sqlite3_bind_int64(stmt.get(), 4, nowMs());
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error("QlibStateStore step ensureAdapterRuntimeStateLocked failed");
    }
}

std::string QlibStateStore::promotionProfileNameLocked() const {
    const char* sql =
        "SELECT promotion_profile FROM qlib_adapter_runtime_state "
        "WHERE adapter_id = ? AND interval = ?;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        throw std::runtime_error("QlibStateStore prepare promotionProfileNameLocked failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    bindText(stmt.get(), 1, m_config.modelId);
    bindText(stmt.get(), 2, m_config.interval);
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        const auto profile = columnText(stmt.get(), 0);
        return profile.empty() ? "default" : profile;
    }
    if (rc == SQLITE_DONE) {
        return "default";
    }
    throw std::runtime_error("QlibStateStore step promotionProfileNameLocked failed");
}

std::string QlibStateStore::modeToDb(ExecutionMode mode) {
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
    if (modeText == "shadow_only") {
        return ExecutionMode::ShadowOnly;
    }
    return ExecutionMode::Shadow;
}

} // namespace orchestration
