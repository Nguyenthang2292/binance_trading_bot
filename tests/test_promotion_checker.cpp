#include <gtest/gtest.h>

#include "orchestration/promotion_checker.h"
#include "orchestration/qlib_state_store.h"
#include "orchestration/shadow_metrics_recorder.h"

#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#elif __has_include(<winsqlite/winsqlite3.h>)
#include <winsqlite/winsqlite3.h>
#else
#error "SQLite headers not found"
#endif

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
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
    const auto path = base / ("promotion_checker_test_" + suffix);
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

std::string scalarText(const std::string& dbPath, const char* sql) {
    sqlite3* rawDb = nullptr;
    if (sqlite3_open(dbPath.c_str(), &rawDb) != SQLITE_OK || rawDb == nullptr) {
        throw std::runtime_error("open scalar db failed");
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        throw std::runtime_error("prepare scalar text failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return {};
    }
    const char* raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    return raw ? std::string(raw) : std::string{};
}

int64_t scalarInt64(const std::string& dbPath, const char* sql) {
    sqlite3* rawDb = nullptr;
    if (sqlite3_open(dbPath.c_str(), &rawDb) != SQLITE_OK || rawDb == nullptr) {
        throw std::runtime_error("open scalar db failed");
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        throw std::runtime_error("prepare scalar int failed");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return 0;
    }
    return sqlite3_column_int64(stmt.get(), 0);
}

void insertOutcome(
    const std::string& dbPath,
    std::string_view modelId,
    std::string_view interval,
    int64_t asofOpenMs,
    int64_t maturedAtMs,
    double netReturn,
    int hit) {
    sqlite3* rawDb = nullptr;
    ASSERT_EQ(sqlite3_open(dbPath.c_str(), &rawDb), SQLITE_OK);
    ASSERT_NE(rawDb, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);

    const std::string shadowId = std::string("shadow_") + std::to_string(asofOpenMs);
    const char* insertSignal =
        "INSERT OR REPLACE INTO qlib_shadow_signals("
        "shadow_id, model_id, run_id, symbol, interval, asof_open_time_ms, generated_at_ms, horizon_bars,"
        "score, score_percentile, direction, confidence, execution_mode, blocked_stage, would_place_order,"
        "current_price, atr, reason, captured_at_ms"
        ") VALUES(?, ?, '', 'BTCUSDT', ?, ?, ?, 1, 0.0, NULL, 'long', 0.9, 'shadow', NULL, 1, 0.0, 0.0, '', ?);";
    sqlite3_stmt* signalStmtRaw = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db.get(), insertSignal, -1, &signalStmtRaw, nullptr), SQLITE_OK);
    ASSERT_NE(signalStmtRaw, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> signalStmt(signalStmtRaw, sqlite3_finalize);
    sqlite3_bind_text(signalStmt.get(), 1, shadowId.c_str(), static_cast<int>(shadowId.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(signalStmt.get(), 2, modelId.data(), static_cast<int>(modelId.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(signalStmt.get(), 3, interval.data(), static_cast<int>(interval.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(signalStmt.get(), 4, asofOpenMs);
    sqlite3_bind_int64(signalStmt.get(), 5, asofOpenMs);
    sqlite3_bind_int64(signalStmt.get(), 6, maturedAtMs);
    ASSERT_EQ(sqlite3_step(signalStmt.get()), SQLITE_DONE);

    const char* insertOutcomeSql =
        "INSERT OR REPLACE INTO qlib_shadow_outcomes("
        "shadow_id, raw_return, direction_return, net_return, hit, matured_at_ms"
        ") VALUES(?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* outcomeStmtRaw = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db.get(), insertOutcomeSql, -1, &outcomeStmtRaw, nullptr), SQLITE_OK);
    ASSERT_NE(outcomeStmtRaw, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> outcomeStmt(outcomeStmtRaw, sqlite3_finalize);
    sqlite3_bind_text(outcomeStmt.get(), 1, shadowId.c_str(), static_cast<int>(shadowId.size()), SQLITE_TRANSIENT);
    sqlite3_bind_double(outcomeStmt.get(), 2, netReturn);
    sqlite3_bind_double(outcomeStmt.get(), 3, netReturn);
    sqlite3_bind_double(outcomeStmt.get(), 4, netReturn);
    sqlite3_bind_int(outcomeStmt.get(), 5, hit);
    sqlite3_bind_int64(outcomeStmt.get(), 6, maturedAtMs);
    ASSERT_EQ(sqlite3_step(outcomeStmt.get()), SQLITE_DONE);
}

struct TestContext {
    std::filesystem::path tempDir;
    TempDirGuard guard;
    std::string dbPath;
    std::shared_ptr<orchestration::QlibStateStore> stateStore;

    TestContext()
        : tempDir(makeTempDir()),
          guard{tempDir},
          dbPath((tempDir / "predictions.db").string()) {
        orchestration::QlibStateStoreConfig sc;
        sc.dbPath = dbPath;
        sc.modelId = "lightgbm_1h_v1";
        sc.interval = "1h";
        stateStore = orchestration::QlibStateStore::create(sc);
        stateStore->initializeSchema();
        stateStore->initializeRuntimeStateIfMissing();

        orchestration::ShadowMetricsConfig sh;
        sh.dbPath = dbPath;
        sh.modelId = "lightgbm_1h_v1";
        sh.interval = "1h";
        sh.horizonBars = 1;
        orchestration::ShadowMetricsRecorder recorder(sh);
        recorder.initializeSchema();
    }
};

void seedOutcomes(
    const std::string& dbPath,
    int count,
    double upReturn,
    double downReturn,
    int hitValue) {
    const int64_t base = 1'700'000'000'000LL;
    for (int i = 0; i < count; ++i) {
        const int64_t asof = base + static_cast<int64_t>(i) * 3'600'000LL;
        const int64_t matured = asof + 3'600'000LL;
        const double r = (i % 2 == 0) ? upReturn : downReturn;
        insertOutcome(dbPath, "lightgbm_1h_v1", "1h", asof, matured, r, hitValue);
    }
}

void installProfile(
    const std::string& dbPath,
    const std::string& profileName,
    const std::string& profileJson) {
    sqlite3* rawDb = nullptr;
    ASSERT_EQ(sqlite3_open(dbPath.c_str(), &rawDb), SQLITE_OK);
    ASSERT_NE(rawDb, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);
    const char* insertSql =
        "INSERT INTO qlib_promotion_profiles(profile_name, qlib_class, profile_json, updated_at_ms) "
        "VALUES(?, 'qlib.contrib.strategy.signal_strategy.TopkDropoutStrategy', ?, 1700000000000) "
        "ON CONFLICT(profile_name) DO UPDATE SET profile_json=excluded.profile_json;";
    sqlite3_stmt* rawStmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db.get(), insertSql, -1, &rawStmt, nullptr), SQLITE_OK);
    ASSERT_NE(rawStmt, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    sqlite3_bind_text(stmt.get(), 1, profileName.c_str(), static_cast<int>(profileName.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, profileJson.c_str(), static_cast<int>(profileJson.size()), SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt.get()), SQLITE_DONE);

    const std::string updateSql =
        "UPDATE qlib_adapter_runtime_state SET promotion_profile='" + profileName +
        "' WHERE adapter_id='lightgbm_1h_v1' AND interval='1h';";
    execOrThrow(db.get(), updateSql.c_str());
}

} // namespace

TEST(PromotionCheckerTest, NotEnoughData) {
    TestContext ctx;
    seedOutcomes(ctx.dbPath, 10, 0.002, 0.001, 1);
    orchestration::PromotionChecker checker({.minCandles = 20, .minSharpe = 0.5, .minHitRate = 0.52, .lookbackCandles = 336});
    const auto result = checker.evaluate(*ctx.stateStore);
    EXPECT_EQ(result, orchestration::PromotionChecker::Result::NotEnoughData);
}

TEST(PromotionCheckerTest, BelowThreshold) {
    TestContext ctx;
    seedOutcomes(ctx.dbPath, 200, 0.0001, -0.0001, 0);
    orchestration::PromotionChecker checker({.minCandles = 100, .minSharpe = 0.5, .minHitRate = 0.52, .lookbackCandles = 336});
    const auto result = checker.evaluate(*ctx.stateStore);
    EXPECT_EQ(result, orchestration::PromotionChecker::Result::BelowThreshold);
}

TEST(PromotionCheckerTest, PromotesToCanary) {
    TestContext ctx;
    seedOutcomes(ctx.dbPath, 220, 0.004, 0.002, 1);
    orchestration::PromotionChecker checker({.minCandles = 100, .minSharpe = 0.5, .minHitRate = 0.52, .lookbackCandles = 336});
    const auto result = checker.evaluate(*ctx.stateStore);
    EXPECT_EQ(result, orchestration::PromotionChecker::Result::PromotedCanary);
    EXPECT_EQ(ctx.stateStore->snapshot().mode, orchestration::ExecutionMode::LiveCanary);
    EXPECT_EQ(
        scalarText(ctx.dbPath, "SELECT execution_mode FROM qlib_adapter_runtime_state WHERE adapter_id='lightgbm_1h_v1';"),
        "live_canary");
    EXPECT_EQ(
        scalarText(ctx.dbPath, "SELECT promoted_by FROM qlib_adapter_runtime_state WHERE adapter_id='lightgbm_1h_v1';"),
        "promotion_checker");
    EXPECT_GT(
        scalarInt64(ctx.dbPath, "SELECT COALESCE(promoted_at_ms, 0) FROM qlib_adapter_runtime_state WHERE adapter_id='lightgbm_1h_v1';"),
        0);
    EXPECT_EQ(
        scalarText(ctx.dbPath, "SELECT decision FROM qlib_promotion_evaluations ORDER BY evaluated_at_ms DESC LIMIT 1;"),
        "promoted_canary");
}

TEST(PromotionCheckerTest, PromotesToLive) {
    TestContext ctx;
    ASSERT_TRUE(ctx.stateStore->setExecutionMode(orchestration::ExecutionMode::LiveCanary));
    seedOutcomes(ctx.dbPath, 220, 0.004, 0.002, 1);
    orchestration::PromotionChecker checker({.minCandles = 100, .minSharpe = 0.5, .minHitRate = 0.52, .lookbackCandles = 336});
    const auto result = checker.evaluate(*ctx.stateStore);
    EXPECT_EQ(result, orchestration::PromotionChecker::Result::PromotedLive);
    EXPECT_EQ(ctx.stateStore->snapshot().mode, orchestration::ExecutionMode::Live);
}

TEST(PromotionCheckerTest, AlreadyLive) {
    TestContext ctx;
    ASSERT_TRUE(ctx.stateStore->setExecutionMode(orchestration::ExecutionMode::Live));
    seedOutcomes(ctx.dbPath, 220, 0.004, 0.002, 1);
    orchestration::PromotionChecker checker({.minCandles = 100, .minSharpe = 0.5, .minHitRate = 0.52, .lookbackCandles = 336});
    const auto result = checker.evaluate(*ctx.stateStore);
    EXPECT_EQ(result, orchestration::PromotionChecker::Result::AlreadyLive);
    EXPECT_EQ(ctx.stateStore->snapshot().mode, orchestration::ExecutionMode::Live);
}

TEST(PromotionCheckerTest, HitRateAloneNotSufficient) {
    TestContext ctx;
    seedOutcomes(ctx.dbPath, 220, 0.0, 0.0, 1);
    orchestration::PromotionChecker checker({.minCandles = 100, .minSharpe = 0.5, .minHitRate = 0.52, .lookbackCandles = 336});
    const auto result = checker.evaluate(*ctx.stateStore);
    EXPECT_EQ(result, orchestration::PromotionChecker::Result::BelowThreshold);
}

TEST(PromotionCheckerTest, SharpeAloneNotSufficient) {
    TestContext ctx;
    seedOutcomes(ctx.dbPath, 220, 0.004, 0.002, 0);
    orchestration::PromotionChecker checker({.minCandles = 100, .minSharpe = 0.5, .minHitRate = 0.52, .lookbackCandles = 336});
    const auto result = checker.evaluate(*ctx.stateStore);
    EXPECT_EQ(result, orchestration::PromotionChecker::Result::BelowThreshold);
}

TEST(PromotionCheckerTest, UnsupportedIntervalDoesNotAnnualizeSharpe) {
    TestContext ctx;
    const int64_t base = 1'700'000'000'000LL;
    insertOutcome(ctx.dbPath, "lightgbm_1h_v1", "1w", base, base + 1, 0.01, 1);
    insertOutcome(ctx.dbPath, "lightgbm_1h_v1", "1w", base + 1, base + 2, -0.005, 0);

    orchestration::PromotionChecker checker({.minCandles = 1, .minSharpe = 0.5, .minHitRate = 0.52, .lookbackCandles = 10});
    const auto stats = checker.computeStats(ctx.dbPath, "lightgbm_1h_v1", "1w");
    EXPECT_TRUE(std::isnan(stats.sharpe));
}

TEST(PromotionCheckerTest, PromotionProfileOverridesConfigThresholds) {
    TestContext ctx;
    seedOutcomes(ctx.dbPath, 220, 0.004, 0.002, 1);
    installProfile(
        ctx.dbPath,
        "fast_profile",
        R"json({"min_shadow_signals":100,"min_sharpe":0.5,"min_hit_rate":0.52,"min_mean_net_return_bps":0.0,"lookback_candles":336})json");

    orchestration::PromotionChecker checker({.minCandles = 500, .minSharpe = 99.0, .minHitRate = 0.99, .lookbackCandles = 336});
    const auto result = checker.evaluate(*ctx.stateStore);

    EXPECT_EQ(result, orchestration::PromotionChecker::Result::PromotedCanary);
    EXPECT_EQ(ctx.stateStore->snapshot().mode, orchestration::ExecutionMode::LiveCanary);
    EXPECT_EQ(
        scalarText(ctx.dbPath, "SELECT profile_name FROM qlib_promotion_evaluations ORDER BY evaluated_at_ms DESC LIMIT 1;"),
        "fast_profile");
}

TEST(PromotionCheckerTest, PromotionProfileCanBlockAndAuditsFailureReason) {
    TestContext ctx;
    seedOutcomes(ctx.dbPath, 220, 0.004, 0.002, 1);
    installProfile(
        ctx.dbPath,
        "strict_profile",
        R"json({"min_shadow_signals":500,"min_sharpe":0.5,"min_hit_rate":0.52,"lookback_candles":336})json");

    orchestration::PromotionChecker checker({.minCandles = 100, .minSharpe = 0.5, .minHitRate = 0.52, .lookbackCandles = 336});
    const auto result = checker.evaluate(*ctx.stateStore);

    EXPECT_EQ(result, orchestration::PromotionChecker::Result::NotEnoughData);
    EXPECT_EQ(ctx.stateStore->snapshot().mode, orchestration::ExecutionMode::Shadow);
    EXPECT_EQ(
        scalarText(ctx.dbPath, "SELECT decision FROM qlib_promotion_evaluations ORDER BY evaluated_at_ms DESC LIMIT 1;"),
        "not_enough_data");
    const auto reason = scalarText(
        ctx.dbPath,
        "SELECT last_failure_reason FROM qlib_adapter_runtime_state WHERE adapter_id='lightgbm_1h_v1';");
    EXPECT_NE(reason.find("not_enough_data"), std::string::npos);
}

TEST(PromotionCheckerTest, MissingPromotionProfileBlocksPromotion) {
    TestContext ctx;
    seedOutcomes(ctx.dbPath, 220, 0.004, 0.002, 1);

    sqlite3* rawDb = nullptr;
    ASSERT_EQ(sqlite3_open(ctx.dbPath.c_str(), &rawDb), SQLITE_OK);
    ASSERT_NE(rawDb, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);
    execOrThrow(
        db.get(),
        "UPDATE qlib_adapter_runtime_state "
        "SET promotion_profile='missing_profile' "
        "WHERE adapter_id='lightgbm_1h_v1' AND interval='1h';");

    orchestration::PromotionChecker checker({.minCandles = 100, .minSharpe = 0.5, .minHitRate = 0.52, .lookbackCandles = 336});
    const auto result = checker.evaluate(*ctx.stateStore);

    EXPECT_EQ(result, orchestration::PromotionChecker::Result::BelowThreshold);
    EXPECT_EQ(ctx.stateStore->snapshot().mode, orchestration::ExecutionMode::Shadow);
    EXPECT_EQ(
        scalarText(ctx.dbPath, "SELECT decision FROM qlib_promotion_evaluations ORDER BY evaluated_at_ms DESC LIMIT 1;"),
        "profile_error");
    EXPECT_EQ(
        scalarText(ctx.dbPath, "SELECT last_failure_reason FROM qlib_adapter_runtime_state WHERE adapter_id='lightgbm_1h_v1';"),
        "profile_not_found");
}

TEST(QlibStateStoreTest, LoadsIndependentAdapterRuntimeStates) {
    TestContext ctx;

    sqlite3* rawDb = nullptr;
    ASSERT_EQ(sqlite3_open(ctx.dbPath.c_str(), &rawDb), SQLITE_OK);
    ASSERT_NE(rawDb, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);
    execOrThrow(
        db.get(),
        "INSERT INTO qlib_adapter_runtime_state("
        "adapter_id, interval, execution_mode, promotion_profile, active_run_id, state_version, updated_at_ms"
        ") VALUES"
        "('adapter_a', '1h', 'live_canary', 'default', 'run-a', 7, 1700000000000),"
        "('adapter_b', '1h', 'shadow', 'default', 'run-b', 3, 1700000000000);");

    const auto adapterA = ctx.stateStore->snapshotForAdapter("adapter_a", "1h");
    EXPECT_TRUE(adapterA.available);
    EXPECT_EQ(adapterA.mode, orchestration::ExecutionMode::LiveCanary);
    EXPECT_EQ(adapterA.activeRunId, "run-a");
    EXPECT_EQ(adapterA.stateVersion, 7);

    const auto adapterB = ctx.stateStore->snapshotForAdapter("adapter_b", "1h");
    EXPECT_TRUE(adapterB.available);
    EXPECT_EQ(adapterB.mode, orchestration::ExecutionMode::Shadow);
    EXPECT_EQ(adapterB.activeRunId, "run-b");

    const auto missing = ctx.stateStore->snapshotForAdapter("missing_adapter", "1h");
    EXPECT_FALSE(missing.available);
    EXPECT_EQ(missing.mode, orchestration::ExecutionMode::Disabled);
}

TEST(QlibStateStoreTest, SeedsLegacyAndAdapterRuntimeModesFromConfigDefaults) {
    TestContext ctx;
    orchestration::QlibStateStoreConfig sc;
    sc.dbPath = (ctx.tempDir / "seeded.db").string();
    sc.modelId = "lightgbm_1h_v1";
    sc.interval = "1h";
    auto stateStore = orchestration::QlibStateStore::create(sc);
    stateStore->initializeSchema();
    stateStore->initializeRuntimeStateIfMissing(orchestration::ExecutionMode::Live);
    stateStore->initializeAdapterRuntimeStatesIfMissing({
        {
            .adapterId = "adapter_live",
            .interval = "1h",
            .executionMode = orchestration::ExecutionMode::Live,
            .promotionProfile = "alpha_profile",
        },
        {
            .adapterId = "adapter_shadow",
            .interval = "1h",
            .executionMode = orchestration::ExecutionMode::Shadow,
            .promotionProfile = "default",
        },
    });

    EXPECT_EQ(stateStore->snapshot().mode, orchestration::ExecutionMode::Live);
    EXPECT_EQ(
        stateStore->snapshotForAdapter("adapter_live", "1h").mode,
        orchestration::ExecutionMode::Live);
    EXPECT_EQ(
        stateStore->snapshotForAdapter("adapter_shadow", "1h").mode,
        orchestration::ExecutionMode::Shadow);

    stateStore->initializeAdapterRuntimeStatesIfMissing({
        {
            .adapterId = "adapter_live",
            .interval = "1h",
            .executionMode = orchestration::ExecutionMode::Disabled,
            .promotionProfile = "changed",
        },
    });
    EXPECT_EQ(
        stateStore->snapshotForAdapter("adapter_live", "1h").mode,
        orchestration::ExecutionMode::Live);
}
