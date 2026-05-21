#include "strategy/istrategy.h"
#include "strategy/strategy_config.h"
#include "orchestration/sqlite_helpers.h"

#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#elif __has_include(<winsqlite/winsqlite3.h>)
#include <winsqlite/winsqlite3.h>
#else
#error "SQLite headers not found"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kExpectedSchemaVersion = 7;

const std::vector<std::string> kDefaultIntervals{"1h"};

struct QlibAdapterParams {
    std::string source{"sqlite"};
    std::string dbPath{"data/qlib_predictions.db"};
    std::string strategyId{""};
    std::string universeHash{""};
    int maxArtifactAgeSeconds{0};
    int maxDataAgeSeconds{0};
    bool universeHashStrict{true};
    bool dryRun{false};
};

struct DecisionRow {
    std::string runId;
    std::string modelId;
    std::string modelRunId;
    std::string universeHash;
    std::string action;
    std::string direction;
    double targetWeight{0.0};
    double score{0.0};
    double scorePercentile{0.0};
    double confidence{0.0};
    std::string reason;
    int64_t asofOpenTimeMs{0};
    int64_t generatedAtMs{0};
};

struct AdapterRuntimeState {
    bool available{false};
    std::string mode{"disabled"};
    std::string activeRunId;
    int stateVersion{0};
};

struct QueryResult {
    bool ok{true};
    std::optional<DecisionRow> row;
    std::string error;
};

struct RuntimeStateQueryResult {
    bool ok{true};
    std::optional<AdapterRuntimeState> state;
    std::string error;
};

int64_t nowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

const nlohmann::json& paramsObject(const nlohmann::json& j) {
    static const nlohmann::json empty = nlohmann::json::object();
    if (!j.contains("params") || !j.at("params").is_object()) {
        return empty;
    }
    return j.at("params");
}

strategy::StrategyConfig parseConfig(const nlohmann::json& j) {
    strategy::StrategyConfig cfg;
    cfg.name = j.value("name", "Qlib Strategy Adapter");
    cfg.type = j.value("type", "qlib_strategy_signal");
    cfg.intervals = j.value("intervals", kDefaultIntervals);
    if (cfg.intervals.empty()) {
        cfg.intervals = kDefaultIntervals;
    }
    cfg.scanInterval = std::chrono::seconds(j.value("scan_interval_seconds", 900));
    cfg.maxHoldDuration = std::chrono::seconds(j.value("max_hold_duration_seconds", 86400));
    cfg.riskPct = j.value("risk_pct", 0.01);
    cfg.slMultiplier = j.value("sl_multiplier", 1.5);
    cfg.tpMultiplier = j.value("tp_multiplier", 3.0);
    cfg.takeProfitPercent = j.value("takeProfitPercent", j.value("take_profit_percent", 20.0));
    cfg.leverage = j.value("leverage", 10);
    cfg.minNotional = j.value("min_notional", 1.0);
    cfg.atrPeriod = j.value("atr_period", 14);
    cfg.minConfidence = j.value("min_confidence", 0.0);
    cfg.priority = j.value("priority", 1000);
    if (j.contains("max_concurrent_positions") && j.at("max_concurrent_positions").is_number_integer()) {
        cfg.maxConcurrentPositions = j.at("max_concurrent_positions").get<int>();
    }
    if (j.contains("max_total_risk_pct") && j.at("max_total_risk_pct").is_number()) {
        cfg.maxTotalRiskPct = j.at("max_total_risk_pct").get<double>();
    }
    return cfg;
}

QlibAdapterParams parseParams(const nlohmann::json& j) {
    QlibAdapterParams p;
    const auto& params = paramsObject(j);
    p.source = params.value("source", p.source);
    p.dbPath = params.value("db_path", p.dbPath);
    p.strategyId = params.value("strategy_id", p.strategyId);
    p.universeHash = params.value("universe_hash", p.universeHash);
    p.maxArtifactAgeSeconds = params.value("max_artifact_age_seconds", p.maxArtifactAgeSeconds);
    p.maxDataAgeSeconds = params.value("max_data_age_seconds", p.maxDataAgeSeconds);
    p.universeHashStrict = params.value("universe_hash_strict", p.universeHashStrict);
    p.dryRun = params.value("dry_run", p.dryRun);
    return p;
}

int intervalSeconds(std::string_view interval) {
    if (interval.empty()) {
        return 3600;
    }
    const char unit = interval.back();
    std::string numberPart(interval);
    if (!std::isdigit(static_cast<unsigned char>(unit))) {
        numberPart.pop_back();
    }
    if (numberPart.empty()) {
        return 3600;
    }
    const int value = std::max(1, std::stoi(numberPart));
    switch (unit) {
        case 'm':
            return value * 60;
        case 'h':
            return value * 3600;
        case 'd':
            return value * 86400;
        case 'w':
            return value * 7 * 86400;
        default:
            return value;
    }
}

void applyDerivedDefaults(const strategy::StrategyConfig& cfg, QlibAdapterParams& params) {
    const std::string& interval = cfg.intervals.empty() ? kDefaultIntervals.front() : cfg.intervals.front();
    const int baseSeconds = intervalSeconds(interval);
    if (params.maxArtifactAgeSeconds <= 0) {
        params.maxArtifactAgeSeconds = baseSeconds * 2;
    }
    if (params.maxDataAgeSeconds <= 0) {
        params.maxDataAgeSeconds = (baseSeconds * 3) / 2;
    }
}

void validateConfig(const strategy::StrategyConfig& cfg, const QlibAdapterParams& params) {
    if (params.source != "sqlite") {
        throw std::invalid_argument("params.source must be sqlite");
    }
    if (params.dbPath.empty()) {
        throw std::invalid_argument("params.db_path must be non-empty");
    }
    if (params.strategyId.empty()) {
        throw std::invalid_argument("params.strategy_id must be non-empty");
    }
    if (params.universeHashStrict && (params.universeHash.empty() || params.universeHash == "default")) {
        throw std::invalid_argument("params.universe_hash must be set when universe_hash_strict=true");
    }
    if (cfg.maxConcurrentPositions.has_value() && cfg.maxConcurrentPositions.value() <= 0) {
        throw std::invalid_argument("max_concurrent_positions must be > 0");
    }
    if (cfg.maxTotalRiskPct.has_value() && cfg.maxTotalRiskPct.value() <= 0.0) {
        throw std::invalid_argument("max_total_risk_pct must be > 0");
    }
}

void configureReadConnection(sqlite3* db) {
    sqlite3_busy_timeout(db, 5000);
    std::string error;
    (void)orchestration::sqlite_helpers::execSql(db, "PRAGMA foreign_keys = ON;", error);
}

std::string buildReason(const QlibAdapterParams& params, const DecisionRow& row, int64_t artifactAgeSeconds, int64_t dataAgeSeconds) {
    std::ostringstream out;
    out << "qlib_adapter=" << params.strategyId
        << " run_id=" << row.runId
        << " model_id=" << row.modelId
        << " model_run_id=" << row.modelRunId
        << " target_weight=" << row.targetWeight
        << " score=" << row.score
        << " score_percentile=" << row.scorePercentile
        << " artifact_age_s=" << artifactAgeSeconds
        << " data_age_s=" << dataAgeSeconds;
    if (!row.reason.empty()) {
        out << " " << row.reason;
    }
    return out.str();
}

void checkSchemaAndUniverse(
    const std::string& dbPath,
    const std::string& strategyId,
    const std::string& expectedUniverse,
    bool universeHashStrict) {
    sqlite3* rawDb = nullptr;
    const int openRc = sqlite3_open_v2(dbPath.c_str(), &rawDb, SQLITE_OPEN_READONLY, nullptr);
    if (openRc != SQLITE_OK || rawDb == nullptr) {
        if (rawDb) sqlite3_close(rawDb);
        throw std::runtime_error("Failed to open DB for schema check");
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);
    configureReadConnection(db.get());

    int version = orchestration::sqlite_helpers::readUserVersion(db.get());
    if (version != kExpectedSchemaVersion) {
        throw std::runtime_error("schema version mismatch: expected " + std::to_string(kExpectedSchemaVersion) + " got " + std::to_string(version));
    }

    if (!expectedUniverse.empty()) {
        const char* sql =
            "SELECT universe_hash FROM qlib_strategy_runs "
            "WHERE strategy_id = ? AND status = 'succeeded' "
            "ORDER BY completed_at_ms DESC, started_at_ms DESC LIMIT 1;";
        sqlite3_stmt* rawStmt = nullptr;
        if (sqlite3_prepare_v2(db.get(), sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
            throw std::runtime_error("failed to prepare universe hash check: " + std::string(sqlite3_errmsg(db.get())));
        }
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
        sqlite3_bind_text(stmt.get(), 1, strategyId.c_str(), static_cast<int>(strategyId.size()), SQLITE_TRANSIENT);
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            throw std::runtime_error("no succeeded qlib_strategy_runs row for universe check");
        }
        if (rc != SQLITE_ROW) {
            throw std::runtime_error("failed to read universe hash: " + std::string(sqlite3_errmsg(db.get())));
        }
        std::string actualHash = orchestration::sqlite_helpers::columnText(stmt.get(), 0);
        if (actualHash != expectedUniverse && (universeHashStrict || actualHash != "default")) {
            throw std::runtime_error("universe hash mismatch: expected " + expectedUniverse + " got " + actualHash);
        }
    }
}

RuntimeStateQueryResult queryRuntimeState(
    const std::string& dbPath,
    const std::string& strategyId,
    std::string_view interval) {
    RuntimeStateQueryResult out;
    sqlite3* rawDb = nullptr;
    const int openRc = sqlite3_open_v2(dbPath.c_str(), &rawDb, SQLITE_OPEN_READONLY, nullptr);
    if (openRc != SQLITE_OK || rawDb == nullptr) {
        out.ok = false;
        out.error = rawDb ? sqlite3_errmsg(rawDb) : "sqlite3_open_v2 failed";
        if (rawDb) {
            sqlite3_close(rawDb);
        }
        return out;
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);
    configureReadConnection(db.get());

    const char* sql =
        "SELECT execution_mode, active_run_id, state_version "
        "FROM qlib_adapter_runtime_state "
        "WHERE adapter_id = ? AND interval = ? "
        "LIMIT 1;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        out.ok = false;
        out.error = sqlite3_errmsg(db.get());
        return out;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);

    sqlite3_bind_text(stmt.get(), 1, strategyId.c_str(), static_cast<int>(strategyId.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, interval.data(), static_cast<int>(interval.size()), SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        out.ok = false;
        out.error = "adapter runtime state not found";
        return out;
    }
    if (rc != SQLITE_ROW) {
        out.ok = false;
        out.error = sqlite3_errmsg(db.get());
        return out;
    }

    AdapterRuntimeState state;
    state.mode = orchestration::sqlite_helpers::columnText(stmt.get(), 0);
    state.activeRunId = orchestration::sqlite_helpers::columnText(stmt.get(), 1);
    state.stateVersion = sqlite3_column_int(stmt.get(), 2);
    state.available = true;
    out.state = state;
    return out;
}

AdapterRuntimeState readRuntimeState(
    const std::string& dbPath,
    const std::string& strategyId,
    std::string_view interval,
    std::string& error) {
    AdapterRuntimeState state;
    const RuntimeStateQueryResult qr = queryRuntimeState(dbPath, strategyId, interval);
    if (!qr.ok || !qr.state.has_value()) {
        error = qr.error.empty() ? "adapter runtime state not found" : qr.error;
        return state;
    }
    return *qr.state;
}

bool isLiveMode(std::string_view mode) {
    return mode == "live" || mode == "live_canary";
}

bool isShadowMode(std::string_view mode) {
    return mode == "shadow" || mode == "shadow_only";
}

QueryResult queryLatestDecision(
    const std::string& dbPath,
    const std::string& strategyId,
    std::string_view symbol,
    std::string_view interval) {
    QueryResult out;
    sqlite3* rawDb = nullptr;
    const int openRc = sqlite3_open_v2(dbPath.c_str(), &rawDb, SQLITE_OPEN_READONLY, nullptr);
    if (openRc != SQLITE_OK || rawDb == nullptr) {
        out.ok = false;
        out.error = rawDb ? sqlite3_errmsg(rawDb) : "sqlite3_open_v2 failed";
        if (rawDb) {
            sqlite3_close(rawDb);
        }
        return out;
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);
    configureReadConnection(db.get());

    const char* sql =
        "SELECT d.run_id, d.model_id, d.model_run_id, r.universe_hash, "
        "d.action, d.direction, d.target_weight, d.score, d.score_percentile, "
        "d.confidence, d.reason, d.asof_open_time_ms, d.generated_at_ms "
        "FROM qlib_strategy_decisions d "
        "JOIN qlib_strategy_runs r ON r.run_id = d.run_id AND r.status = 'succeeded' "
        "WHERE d.strategy_id = ? AND d.symbol = ? AND d.interval = ? "
        "ORDER BY d.generated_at_ms DESC "
        "LIMIT 1;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        out.ok = false;
        out.error = sqlite3_errmsg(db.get());
        return out;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);

    sqlite3_bind_text(stmt.get(), 1, strategyId.c_str(), static_cast<int>(strategyId.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, symbol.data(), static_cast<int>(symbol.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, interval.data(), static_cast<int>(interval.size()), SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        out.row = std::nullopt;
        return out;
    }
    if (rc != SQLITE_ROW) {
        out.ok = false;
        out.error = sqlite3_errmsg(db.get());
        return out;
    }

    DecisionRow row;
    row.runId = orchestration::sqlite_helpers::columnText(stmt.get(), 0);
    row.modelId = orchestration::sqlite_helpers::columnText(stmt.get(), 1);
    row.modelRunId = orchestration::sqlite_helpers::columnText(stmt.get(), 2);
    row.universeHash = orchestration::sqlite_helpers::columnText(stmt.get(), 3);
    row.action = orchestration::sqlite_helpers::columnText(stmt.get(), 4);
    row.direction = orchestration::sqlite_helpers::columnText(stmt.get(), 5);
    row.targetWeight = sqlite3_column_type(stmt.get(), 6) == SQLITE_NULL ? 0.0 : sqlite3_column_double(stmt.get(), 6);
    row.score = sqlite3_column_type(stmt.get(), 7) == SQLITE_NULL ? 0.0 : sqlite3_column_double(stmt.get(), 7);
    row.scorePercentile = sqlite3_column_type(stmt.get(), 8) == SQLITE_NULL ? 0.0 : sqlite3_column_double(stmt.get(), 8);
    row.confidence = sqlite3_column_double(stmt.get(), 9);
    row.reason = orchestration::sqlite_helpers::columnText(stmt.get(), 10);
    row.asofOpenTimeMs = sqlite3_column_int64(stmt.get(), 11);
    row.generatedAtMs = sqlite3_column_int64(stmt.get(), 12);
    out.row = row;
    return out;
}

bool intervalConfigured(const strategy::StrategyConfig& cfg, std::string_view interval) {
    return std::find(cfg.intervals.begin(), cfg.intervals.end(), interval) != cfg.intervals.end();
}

class QlibStrategySignalStrategy final : public strategy::IStrategy {
public:
    QlibStrategySignalStrategy(strategy::StrategyConfig cfg, QlibAdapterParams params)
        : m_cfg(std::move(cfg)),
          m_params(std::move(params)) {
        checkSchemaAndUniverse(
            m_params.dbPath,
            m_params.strategyId,
            m_params.universeHash,
            m_params.universeHashStrict);
    }

    const strategy::StrategyConfig& config() const override {
        return m_cfg;
    }

    strategy::Signal evaluate(
        std::string_view symbol,
        std::string_view interval,
        const std::vector<Kline>& klines) const override {
        (void)klines;
        if (!intervalConfigured(m_cfg, interval)) {
            return {};
        }

        std::string runtimeError;
        const AdapterRuntimeState runtimeState =
            readRuntimeState(m_params.dbPath, m_params.strategyId, interval, runtimeError);
        if (!runtimeState.available) {
            return strategy::Signal{.reason = "qlib_adapter=" + m_params.strategyId + " (runtime_state_unavailable: " + runtimeError + ")"};
        }
        if (runtimeState.mode == "disabled") {
            return strategy::Signal{.reason = "qlib_adapter=" + m_params.strategyId + " (execution_mode=disabled)"};
        }

        const QueryResult qr = queryLatestDecision(m_params.dbPath, m_params.strategyId, symbol, interval);
        if (!qr.ok || !qr.row.has_value()) {
            return {};
        }

        const auto& row = *qr.row;
        const int64_t nowMs = nowEpochMs();
        const int64_t artifactAgeSeconds = std::max<int64_t>(0, (nowMs - row.generatedAtMs) / 1000);
        const int64_t dataAgeSeconds = std::max<int64_t>(0, (nowMs - row.asofOpenTimeMs) / 1000);

        std::string reason = buildReason(m_params, row, artifactAgeSeconds, dataAgeSeconds) +
            " execution_mode=" + runtimeState.mode +
            " state_version=" + std::to_string(runtimeState.stateVersion);

        if (!m_params.universeHash.empty() &&
            row.universeHash != m_params.universeHash &&
            (m_params.universeHashStrict || row.universeHash != "default")) {
            return strategy::Signal{.reason = reason + " (universe_mismatch)"};
        }

        if (artifactAgeSeconds > m_params.maxArtifactAgeSeconds) {
            return strategy::Signal{.reason = reason + " (stale artifact)"};
        }
        if (dataAgeSeconds > m_params.maxDataAgeSeconds) {
            return strategy::Signal{.reason = reason + " (stale data)"};
        }
        
        if (row.action != "buy") {
            return strategy::Signal{.reason = reason + " (action=" + row.action + ")"};
        }

        strategy::Signal::Direction direction = strategy::Signal::Direction::None;
        if (row.direction == "long") {
            direction = strategy::Signal::Direction::Long;
        } else {
            return strategy::Signal{.reason = reason + " (invalid_direction=" + row.direction + ")"};
        }

        if (m_params.dryRun || isShadowMode(runtimeState.mode)) {
            return strategy::Signal{
                .direction = strategy::Signal::Direction::None,
                .confidence = row.confidence,
                .reason = reason + (m_params.dryRun ? " (dry run)" : " (shadow_only)"),
                .shadowOnly = true,
                .wouldPlaceOrder = direction != strategy::Signal::Direction::None,
            };
        }

        if (!isLiveMode(runtimeState.mode)) {
            return strategy::Signal{.reason = reason + " (execution_mode_not_live)"};
        }

        return strategy::Signal{
            .direction = direction,
            .confidence = row.confidence,
            .reason = reason,
        };
    }

private:
    strategy::StrategyConfig m_cfg;
    QlibAdapterParams m_params;
};

} // namespace

extern "C" {

__declspec(dllexport) strategy::IStrategy* createStrategy(const char* config_json) {
    try {
        const auto j = nlohmann::json::parse(config_json == nullptr ? "{}" : config_json);
        auto cfg = parseConfig(j);
        auto params = parseParams(j);
        applyDerivedDefaults(cfg, params);
        validateConfig(cfg, params);
        return new QlibStrategySignalStrategy(std::move(cfg), std::move(params));
    } catch (const std::exception& e) {
        std::cerr << "Failed to create qlib_strategy_signal: " << e.what() << std::endl;
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

__declspec(dllexport) void destroyStrategy(strategy::IStrategy* strategy) {
    delete strategy;
}

__declspec(dllexport) const char* strategyType() {
    return "qlib_strategy_signal";
}

__declspec(dllexport) const char* pluginVersion() {
    return "1.1.0";
}

} // extern "C"
