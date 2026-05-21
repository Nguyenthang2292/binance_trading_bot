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
