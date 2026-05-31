#include <gtest/gtest.h>

#include "orchestration/orchestrator_config.h"
#include "orchestration/shadow_metrics_recorder.h"

#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#elif __has_include(<winsqlite/winsqlite3.h>)
#include <winsqlite/winsqlite3.h>
#else
#error "SQLite headers not found"
#endif

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

struct TempDirGuard {
    std::filesystem::path path;
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::filesystem::path makeTempDir() {
    const auto base = std::filesystem::temp_directory_path();
    const auto suffix = std::to_string(
        static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = base / ("shadow_metrics_test_" + suffix);
    std::filesystem::create_directories(path);
    return path;
}

void execOrThrow(sqlite3* db, const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc == SQLITE_OK) {
        return;
    }
    std::string msg = err ? err : "sqlite error";
    if (err) {
        sqlite3_free(err);
    }
    throw std::runtime_error(msg);
}

bool indexExists(const std::string& dbPath, std::string_view indexName) {
    sqlite3* rawDb = nullptr;
    EXPECT_EQ(sqlite3_open(dbPath.c_str(), &rawDb), SQLITE_OK);
    EXPECT_NE(rawDb, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);

    sqlite3_stmt* rawStmt = nullptr;
    EXPECT_EQ(
        sqlite3_prepare_v2(
            db.get(),
            "SELECT 1 FROM sqlite_master WHERE type='index' AND name=?;",
            -1,
            &rawStmt,
            nullptr),
        SQLITE_OK);
    EXPECT_NE(rawStmt, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    sqlite3_bind_text(stmt.get(), 1, indexName.data(), static_cast<int>(indexName.size()), SQLITE_TRANSIENT);
    return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

void insertPrediction(const std::string& dbPath, int64_t asofMs) {
    sqlite3* rawDb = nullptr;
    ASSERT_EQ(sqlite3_open(dbPath.c_str(), &rawDb), SQLITE_OK);
    ASSERT_NE(rawDb, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);
    execOrThrow(
        db.get(),
        "INSERT INTO qlib_predictions("
        "model_id, run_id, symbol, interval, asof_open_time_ms, generated_at_ms, horizon_bars, score, rank, score_percentile"
        ") VALUES('lightgbm_1h_v1', 'run_1', 'BTCUSDT', '1h', 1700000000000, 1700000001000, 1, 0.8, 1, 0.95);");
    (void)asofMs;
}

Kline makeKline(int64_t openMs, double close) {
    Kline kline;
    kline.openTime = openMs;
    kline.closeTime = openMs + 3'599'999LL;
    kline.open = close;
    kline.high = close;
    kline.low = close;
    kline.close = close;
    kline.volume = 1.0;
    kline.quoteVolume = close;
    kline.tradeCount = 1;
    kline.isClosed = true;
    return kline;
}

double scalarDouble(const std::string& dbPath, const char* sql) {
    sqlite3* rawDb = nullptr;
    EXPECT_EQ(sqlite3_open(dbPath.c_str(), &rawDb), SQLITE_OK);
    EXPECT_NE(rawDb, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);

    sqlite3_stmt* rawStmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db.get(), sql, -1, &rawStmt, nullptr), SQLITE_OK);
    EXPECT_NE(rawStmt, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return 0.0;
    }
    return sqlite3_column_double(stmt.get(), 0);
}

std::string scalarText(const std::string& dbPath, const char* sql) {
    sqlite3* rawDb = nullptr;
    EXPECT_EQ(sqlite3_open(dbPath.c_str(), &rawDb), SQLITE_OK);
    EXPECT_NE(rawDb, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);

    sqlite3_stmt* rawStmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db.get(), sql, -1, &rawStmt, nullptr), SQLITE_OK);
    EXPECT_NE(rawStmt, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return {};
    }
    const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    return value ? std::string(value) : std::string{};
}

int64_t scalarInt64(const std::string& dbPath, const char* sql) {
    sqlite3* rawDb = nullptr;
    EXPECT_EQ(sqlite3_open(dbPath.c_str(), &rawDb), SQLITE_OK);
    EXPECT_NE(rawDb, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);

    sqlite3_stmt* rawStmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db.get(), sql, -1, &rawStmt, nullptr), SQLITE_OK);
    EXPECT_NE(rawStmt, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return 0;
    }
    return sqlite3_column_int64(stmt.get(), 0);
}

} // namespace

TEST(ShadowMetricsRecorderTest, InitializeSchemaCreatesIntervalIndexes) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};
    const std::string dbPath = (tmp / "predictions.db").string();

    orchestration::ShadowMetricsConfig cfg;
    cfg.dbPath = dbPath;
    orchestration::ShadowMetricsRecorder recorder(cfg);
    recorder.initializeSchema();

    EXPECT_TRUE(indexExists(dbPath, "idx_qlib_candles_interval_open"));
    EXPECT_TRUE(indexExists(dbPath, "idx_qlib_actual_returns_interval_asof"));
}

TEST(ShadowMetricsRecorderTest, RecordsSignalAndMaturesOutcome) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};
    const std::string dbPath = (tmp / "predictions.db").string();

    orchestration::ShadowMetricsConfig cfg;
    cfg.dbPath = dbPath;
    cfg.modelId = "lightgbm_1h_v1";
    cfg.interval = "1h";
    cfg.horizonBars = 1;
    cfg.costModel.estimatedRoundTripFeeBps = 0.0;
    cfg.costModel.estimatedSlippageBps = 0.0;
    cfg.costModel.estimatedFundingBpsPerDay = 0.0;
    orchestration::ShadowMetricsRecorder recorder(cfg);
    recorder.initializeSchema();

    const int64_t asofMs = 1'700'000'000'000LL;
    insertPrediction(dbPath, asofMs);

    orchestration::ShadowSignalRecord record;
    record.modelId = "lightgbm_1h_v1";
    record.symbol = "BTCUSDT";
    record.interval = "1h";
    record.asofOpenTimeMs = asofMs;
    record.capturedAtMs = asofMs + 1000;
    record.direction = strategy::Signal::Direction::Long;
    record.confidence = 0.9;
    record.executionMode = orchestration::ExecutionMode::Shadow;
    record.wouldPlaceOrder = true;
    recorder.recordShadowSignal(record);

    recorder.onCandleClosed("BTCUSDT", "1h", makeKline(asofMs, 100.0));
    recorder.onCandleClosed("BTCUSDT", "1h", makeKline(asofMs + 3'600'000LL, 105.0));

    EXPECT_NEAR(
        scalarDouble(dbPath, "SELECT net_return FROM qlib_shadow_outcomes LIMIT 1;"),
        0.05,
        1e-9);
    EXPECT_NEAR(
        scalarDouble(dbPath, "SELECT raw_return FROM qlib_actual_returns LIMIT 1;"),
        0.05,
        1e-9);
    EXPECT_NE(
        scalarText(dbPath, "SELECT cost_model_version FROM qlib_shadow_outcomes LIMIT 1;").find("rtf=0"),
        std::string::npos);
}

TEST(ShadowMetricsRecorderTest, OutcomeBackfillIsScopedToRecorderModel) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};
    const std::string dbPath = (tmp / "predictions.db").string();

    orchestration::ShadowMetricsConfig cfg;
    cfg.dbPath = dbPath;
    cfg.modelId = "model_a";
    cfg.interval = "1h";
    cfg.horizonBars = 1;
    cfg.costModel.estimatedRoundTripFeeBps = 0.0;
    cfg.costModel.estimatedSlippageBps = 0.0;
    cfg.costModel.estimatedFundingBpsPerDay = 0.0;
    orchestration::ShadowMetricsRecorder recorder(cfg);
    recorder.initializeSchema();

    const int64_t asofMs = 1'700'000'000'000LL;
    orchestration::ShadowSignalRecord record;
    record.symbol = "BTCUSDT";
    record.interval = "1h";
    record.asofOpenTimeMs = asofMs;
    record.capturedAtMs = asofMs + 1000;
    record.direction = strategy::Signal::Direction::Long;
    record.confidence = 0.9;
    record.executionMode = orchestration::ExecutionMode::Shadow;
    record.wouldPlaceOrder = true;

    record.modelId = "model_a";
    recorder.recordShadowSignal(record);
    record.modelId = "model_b";
    recorder.recordShadowSignal(record);

    recorder.onCandleClosed("BTCUSDT", "1h", makeKline(asofMs, 100.0));
    recorder.onCandleClosed("BTCUSDT", "1h", makeKline(asofMs + 3'600'000LL, 105.0));

    EXPECT_EQ(scalarInt64(dbPath, "SELECT COUNT(*) FROM qlib_shadow_outcomes;"), 1);
    EXPECT_EQ(
        scalarInt64(
            dbPath,
            "SELECT COUNT(*) FROM qlib_shadow_outcomes o "
            "JOIN qlib_shadow_signals s ON s.shadow_id=o.shadow_id "
            "WHERE s.model_id='model_a';"),
        1);
}

TEST(ShadowMetricsRecorderTest, NegativeCostConfigIsClamped) {
    const auto cfg = orchestration::parseOrchestratorConfig(nlohmann::json::parse(R"json({
        "qlib_orchestration": {
            "enabled": true,
            "cost_model": {
                "estimated_round_trip_fee_bps": -1.0,
                "estimated_slippage_bps": -2.0,
                "estimated_funding_bps_per_day": -3.0
            }
        }
    })json"));

    EXPECT_EQ(cfg.shadowMetrics.costModel.estimatedRoundTripFeeBps, 0.0);
    EXPECT_EQ(cfg.shadowMetrics.costModel.estimatedSlippageBps, 0.0);
    EXPECT_EQ(cfg.shadowMetrics.costModel.estimatedFundingBpsPerDay, 0.0);
}

TEST(ShadowMetricsRecorderTest, RepeatedSignalForSameAsofUpdatesSingleShadowRow) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};
    const std::string dbPath = (tmp / "predictions.db").string();

    orchestration::ShadowMetricsConfig cfg;
    cfg.dbPath = dbPath;
    cfg.modelId = "lightgbm_1h_v1";
    cfg.interval = "1h";
    cfg.horizonBars = 1;
    orchestration::ShadowMetricsRecorder recorder(cfg);
    recorder.initializeSchema();

    const int64_t asofMs = 1'700'000'000'000LL;
    insertPrediction(dbPath, asofMs);

    orchestration::ShadowSignalRecord record;
    record.modelId = "lightgbm_1h_v1";
    record.adapterId = "adapter-a";
    record.symbol = "BTCUSDT";
    record.interval = "1h";
    record.asofOpenTimeMs = asofMs;
    record.capturedAtMs = asofMs + 1000;
    record.direction = strategy::Signal::Direction::Long;
    record.confidence = 0.9;
    record.executionMode = orchestration::ExecutionMode::Shadow;
    record.wouldPlaceOrder = true;
    recorder.recordShadowSignal(record);

    record.capturedAtMs = asofMs + 5000;
    record.confidence = 0.6;
    recorder.recordShadowSignal(record);

    EXPECT_EQ(scalarInt64(dbPath, "SELECT COUNT(*) FROM qlib_shadow_signals;"), 1);
    EXPECT_EQ(
        scalarInt64(dbPath, "SELECT captured_at_ms FROM qlib_shadow_signals LIMIT 1;"),
        asofMs + 5000);
}
