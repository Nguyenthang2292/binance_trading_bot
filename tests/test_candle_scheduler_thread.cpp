#include <gtest/gtest.h>

#include "orchestration/candle_scheduler_thread.h"
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
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    const auto path = base / ("candle_scheduler_test_" + suffix);
    std::filesystem::create_directories(path);
    return path;
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

void seedGoodOutcomes(const std::string& dbPath, int count) {
    const int64_t base = 1'700'000'000'000LL;
    for (int i = 0; i < count; ++i) {
        const int64_t asof = base + static_cast<int64_t>(i) * 3'600'000LL;
        const int64_t matured = asof + 3'600'000LL;
        insertOutcome(dbPath, "lightgbm_1h_v1", "1h", asof, matured, 0.003 + (i % 3) * 0.0001, 1);
    }
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
        orchestration::QlibStateStoreConfig stateCfg;
        stateCfg.dbPath = dbPath;
        stateCfg.modelId = "lightgbm_1h_v1";
        stateCfg.interval = "1h";
        stateStore = orchestration::QlibStateStore::create(stateCfg);
        stateStore->initializeSchema();
        stateStore->initializeRuntimeStateIfMissing();

        orchestration::ShadowMetricsConfig shadowCfg;
        shadowCfg.dbPath = dbPath;
        shadowCfg.modelId = "lightgbm_1h_v1";
        shadowCfg.interval = "1h";
        orchestration::ShadowMetricsRecorder shadowRecorder(shadowCfg);
        shadowRecorder.initializeSchema();
    }
};

class FakeProcessRunner final : public orchestration::IProcessRunner {
public:
    explicit FakeProcessRunner(orchestration::ProcessResult result)
        : m_result(std::move(result)) {}

    orchestration::ProcessResult spawnWithRetry(const std::vector<std::string>& cmd) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_calls.push_back(cmd);
        }
        m_cv.notify_all();
        return m_result;
    }

    bool startDaemon(const std::string& name, const std::vector<std::string>& cmd) override { return true; }
    bool isDaemonRunning(const std::string& name) override { return false; }
    void stopDaemon(const std::string& name) override {}

    bool waitForCallCount(size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [&]() { return m_calls.size() >= count; });
    }

    size_t callCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_calls.size();
    }

    std::vector<std::string> callAt(size_t index) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (index >= m_calls.size()) {
            return {};
        }
        return m_calls[index];
    }

private:
    orchestration::ProcessResult m_result;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<std::vector<std::string>> m_calls;
};

orchestration::CandleSchedulerConfig makeCandleConfig(
    const std::filesystem::path& root,
    const std::string& dbPath) {
    orchestration::CandleSchedulerConfig cfg;
    cfg.pythonExe = "python";
    cfg.scriptsDir = "tools/qlib_bridge";
    cfg.dataDir = (root / "data").string();
    cfg.modelDir = (root / "models").string();
    cfg.dbPath = dbPath;
    cfg.modelId = "lightgbm_1h_v1";
    cfg.interval = "1h";
    cfg.postCandleDelaySeconds = 0;
    cfg.readyDir = (root / "ready").string();
    return cfg;
}

std::string readAsOfArg(const std::vector<std::string>& cmd) {
    for (size_t i = 0; i + 1 < cmd.size(); ++i) {
        if (cmd[i] == "--asof-ms") {
            return cmd[i + 1];
        }
    }
    return {};
}

bool commandHasArg(const std::vector<std::string>& cmd, std::string_view token) {
    for (const auto& part : cmd) {
        if (part == token) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(CandleSchedulerThreadTest, Phase3CommandDoesNotUseUnsupportedArgs) {
    TestContext ctx;
    orchestration::PromotionChecker promoter({.minCandles = 1000});
    FakeProcessRunner runner({.exitCode = 0, .timedOut = false, .succeeded = true, .logPath = "p3.log"});

    orchestration::CandleSchedulerThread scheduler(
        makeCandleConfig(ctx.tempDir, ctx.dbPath),
        runner,
        *ctx.stateStore,
        promoter);
    const auto cmd = scheduler.buildPhase3Cmd(1'700'000'000'000LL);
    EXPECT_FALSE(commandHasArg(cmd, "--run-id"));
    EXPECT_FALSE(commandHasArg(cmd, "--horizon-bars"));
}

TEST(CandleSchedulerThreadTest, NotifyQueuesCandle) {
    TestContext ctx;
    orchestration::PromotionChecker promoter({.minCandles = 1000});
    FakeProcessRunner runner({.exitCode = 0, .timedOut = false, .succeeded = true, .logPath = "p3.log"});

    orchestration::CandleSchedulerThread scheduler(
        makeCandleConfig(ctx.tempDir, ctx.dbPath),
        runner,
        *ctx.stateStore,
        promoter);

    scheduler.notifyCandleClose(1'700'000'000'000LL, "BTCUSDT");
    scheduler.notifyCandleClose(1'700'000'003'600LL, "BTCUSDT");

    std::jthread worker([&](std::stop_token st) { scheduler.run(st); });
    ASSERT_TRUE(runner.waitForCallCount(1, std::chrono::milliseconds(2500)));
    worker.request_stop();
    worker.join();

    EXPECT_EQ(runner.callCount(), 1U);
    EXPECT_EQ(readAsOfArg(runner.callAt(0)), "1700000003600");
}

TEST(CandleSchedulerThreadTest, DuplicateNotifyIgnored) {
    TestContext ctx;
    orchestration::PromotionChecker promoter({.minCandles = 1000});
    FakeProcessRunner runner({.exitCode = 0, .timedOut = false, .succeeded = true, .logPath = "p3.log"});

    orchestration::CandleSchedulerThread scheduler(
        makeCandleConfig(ctx.tempDir, ctx.dbPath),
        runner,
        *ctx.stateStore,
        promoter);

    std::jthread worker([&](std::stop_token st) { scheduler.run(st); });
    scheduler.notifyCandleClose(1'700'000'000'000LL, "BTCUSDT");
    scheduler.notifyCandleClose(1'700'000'000'000LL, "BTCUSDT");

    ASSERT_TRUE(runner.waitForCallCount(1, std::chrono::milliseconds(2500)));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    worker.request_stop();
    worker.join();

    EXPECT_EQ(runner.callCount(), 1U);
}

TEST(CandleSchedulerThreadTest, Phase4CheckCalledAfterPhase3Success) {
    TestContext ctx;
    seedGoodOutcomes(ctx.dbPath, 220);
    orchestration::PromotionChecker promoter({.minCandles = 100, .minSharpe = 0.5, .minHitRate = 0.52});
    FakeProcessRunner runner({.exitCode = 0, .timedOut = false, .succeeded = true, .logPath = "p3.log"});

    orchestration::CandleSchedulerThread scheduler(
        makeCandleConfig(ctx.tempDir, ctx.dbPath),
        runner,
        *ctx.stateStore,
        promoter);

    std::jthread worker([&](std::stop_token st) { scheduler.run(st); });
    scheduler.notifyCandleClose(1'700'000'000'000LL, "BTCUSDT");
    ASSERT_TRUE(runner.waitForCallCount(1, std::chrono::milliseconds(2500)));
    worker.request_stop();
    worker.join();

    EXPECT_EQ(ctx.stateStore->snapshot().mode, orchestration::ExecutionMode::LiveCanary);
}

TEST(CandleSchedulerThreadTest, Phase4CheckSkippedAfterPhase3Failure) {
    TestContext ctx;
    seedGoodOutcomes(ctx.dbPath, 220);
    orchestration::PromotionChecker promoter({.minCandles = 100, .minSharpe = 0.5, .minHitRate = 0.52});
    FakeProcessRunner runner({.exitCode = 1, .timedOut = false, .succeeded = false, .logPath = "p3.log"});

    orchestration::CandleSchedulerThread scheduler(
        makeCandleConfig(ctx.tempDir, ctx.dbPath),
        runner,
        *ctx.stateStore,
        promoter);

    std::jthread worker([&](std::stop_token st) { scheduler.run(st); });
    scheduler.notifyCandleClose(1'700'000'000'000LL, "BTCUSDT");
    ASSERT_TRUE(runner.waitForCallCount(1, std::chrono::milliseconds(2500)));
    worker.request_stop();
    worker.join();

    EXPECT_EQ(ctx.stateStore->snapshot().mode, orchestration::ExecutionMode::Shadow);
}
