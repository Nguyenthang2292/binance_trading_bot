#include <gtest/gtest.h>

#include "engine/qlib_execution_planner.h"
#include "orchestration/sqlite_helpers.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

fs::path uniqueDbPath(std::string_view suffix) {
    const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return fs::temp_directory_path() / ("qlib_execution_planner_" + std::string(suffix) + "_" + std::to_string(nowNs) + ".db");
}

template <typename T>
T runAwaitable(boost::asio::io_context& ioc, boost::asio::awaitable<T> task) {
    auto fut = boost::asio::co_spawn(ioc, std::move(task), boost::asio::use_future);
    ioc.run();
    return fut.get();
}

void exec(sqlite3* db, const char* sql) {
    orchestration::sqlite_helpers::execOrThrow(db, sql);
}

void initializeDb(const fs::path& path) {
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open_v2(path.string().c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    exec(db.get(), "PRAGMA user_version = 7;");
    exec(db.get(), "PRAGMA journal_mode = WAL;");
    exec(db.get(), "PRAGMA foreign_keys = ON;");
    exec(db.get(),
         "CREATE TABLE qlib_execution_requests ("
         "request_id TEXT PRIMARY KEY,"
         "symbol TEXT NOT NULL,"
         "side TEXT NOT NULL,"
         "quantity TEXT NOT NULL,"
         "position_side TEXT NOT NULL,"
         "metadata_json TEXT,"
         "status TEXT NOT NULL CHECK (status IN ('pending','succeeded','expired','failed')),"
         "created_at_ms INTEGER NOT NULL,"
         "deadline_ms INTEGER NOT NULL,"
         "error TEXT);");
    exec(db.get(),
         "CREATE TABLE qlib_execution_plans ("
         "plan_id TEXT PRIMARY KEY,"
         "request_id TEXT NOT NULL,"
         "algorithm TEXT NOT NULL,"
         "status TEXT NOT NULL CHECK (status IN ('running','succeeded','failed','expired')),"
         "generated_at_ms INTEGER NOT NULL,"
         "expires_at_ms INTEGER NOT NULL,"
         "total_quantity TEXT NOT NULL,"
         "slice_count INTEGER NOT NULL,"
         "error TEXT,"
         "FOREIGN KEY (request_id) REFERENCES qlib_execution_requests(request_id));");
    exec(db.get(),
         "CREATE TABLE qlib_execution_slices ("
         "slice_id TEXT PRIMARY KEY,"
         "plan_id TEXT NOT NULL,"
         "slice_index INTEGER NOT NULL,"
         "due_at_ms INTEGER NOT NULL,"
         "side TEXT NOT NULL,"
         "quantity TEXT NOT NULL,"
         "status TEXT NOT NULL CHECK (status IN ('pending','submitted','filled','failed','revoked')),"
         "revoked_at_ms INTEGER,"
         "revoke_reason TEXT,"
         "FOREIGN KEY (plan_id) REFERENCES qlib_execution_plans(plan_id));");
    exec(db.get(),
         "CREATE TABLE qlib_strategy_runs ("
         "run_id TEXT PRIMARY KEY,"
         "strategy_id TEXT NOT NULL,"
         "qlib_class TEXT NOT NULL,"
         "config_hash TEXT NOT NULL,"
         "model_id TEXT,"
         "model_run_id TEXT,"
         "interval TEXT NOT NULL,"
         "universe_hash TEXT NOT NULL,"
         "started_at_ms INTEGER NOT NULL,"
         "completed_at_ms INTEGER,"
         "status TEXT NOT NULL CHECK (status IN ('running','succeeded','failed')),"
         "error TEXT);");
    exec(db.get(),
         "CREATE TABLE qlib_strategy_decisions ("
         "strategy_id TEXT NOT NULL,"
         "run_id TEXT NOT NULL,"
         "model_id TEXT,"
         "model_run_id TEXT,"
         "symbol TEXT NOT NULL,"
         "interval TEXT NOT NULL,"
         "asof_open_time_ms INTEGER NOT NULL,"
         "generated_at_ms INTEGER NOT NULL,"
         "action TEXT NOT NULL CHECK (action IN ('buy','hold','none')),"
         "direction TEXT NOT NULL CHECK (direction IN ('long','none')),"
         "target_weight REAL,"
         "score REAL,"
         "score_percentile REAL,"
         "confidence REAL NOT NULL,"
         "reason TEXT,"
         "PRIMARY KEY (strategy_id, symbol, interval, asof_open_time_ms),"
         "FOREIGN KEY (run_id) REFERENCES qlib_strategy_runs(run_id));");
}

std::string firstPendingRequest(sqlite3* db) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT request_id FROM qlib_execution_requests WHERE status='pending' LIMIT 1;", -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        return {};
    }
    return orchestration::sqlite_helpers::columnText(stmt, 0);
}

boost::asio::awaitable<void> insertPlanWhenRequestAppears(std::string dbPath, int sliceCount, std::string sliceSide = "BUY") {
    sqlite3* raw = nullptr;
    sqlite3_open_v2(dbPath.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    sqlite3_busy_timeout(db.get(), 5000);
    std::string requestId;
    for (int i = 0; i < 50 && requestId.empty(); ++i) {
        requestId = firstPendingRequest(db.get());
        if (!requestId.empty()) break;
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(10));
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
    if (requestId.empty()) co_return;
    const auto now = orchestration::sqlite_helpers::nowMs();
    exec(db.get(), "BEGIN IMMEDIATE;");
    sqlite3_stmt* plan = nullptr;
    sqlite3_prepare_v2(db.get(),
        "INSERT INTO qlib_execution_plans(plan_id, request_id, algorithm, status, generated_at_ms, expires_at_ms, total_quantity, slice_count, error) "
        "VALUES('plan-1', ?, 'TWAP', 'succeeded', ?, ?, '0.02', ?, NULL);",
        -1, &plan, nullptr);
    orchestration::sqlite_helpers::bindText(plan, 1, requestId);
    sqlite3_bind_int64(plan, 2, now);
    sqlite3_bind_int64(plan, 3, now + 10'000);
    sqlite3_bind_int(plan, 4, sliceCount);
    sqlite3_step(plan);
    sqlite3_finalize(plan);
    for (int i = 0; i < sliceCount; ++i) {
        sqlite3_stmt* slice = nullptr;
        sqlite3_prepare_v2(db.get(),
            "INSERT INTO qlib_execution_slices(slice_id, plan_id, slice_index, due_at_ms, side, quantity, status, revoked_at_ms, revoke_reason) "
            "VALUES(?, 'plan-1', ?, ?, ?, '0.01', 'pending', NULL, NULL);",
            -1, &slice, nullptr);
        const std::string sliceId = "slice-" + std::to_string(i);
        orchestration::sqlite_helpers::bindText(slice, 1, sliceId);
        sqlite3_bind_int(slice, 2, i);
        sqlite3_bind_int64(slice, 3, now);
        orchestration::sqlite_helpers::bindText(slice, 4, sliceSide);
        sqlite3_step(slice);
        sqlite3_finalize(slice);
    }
    exec(db.get(), "COMMIT;");
}

class RecordingPlanner final : public engine::IExecutionPlanner {
public:
    std::vector<std::string> quantities;

    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> executeMarket(MarketOrderDraft draft) override {
        quantities.emplace_back(draft.quantity.value());
        NormalPlacementResult result;
        result.state = PlacementState::Accepted;
        result.symbol = draft.symbol;
        co_return result;
    }
};

} // namespace

TEST(QlibExecutionPlannerTest, ExecutesReadyPlanSlices) {
    const auto dbPath = uniqueDbPath("slices");
    initializeDb(dbPath);
    RecordingPlanner fallback;
    engine::QlibExecutionPlanner planner(dbPath.string(), fallback);
    boost::asio::io_context ioc;
    boost::asio::co_spawn(ioc, insertPlanWhenRequestAppears(dbPath.string(), 2), boost::asio::detached);
    auto qty = DecimalString::parse("0.02");
    ASSERT_TRUE(qty.has_value());
    auto result = runAwaitable(ioc, planner.executeMarket(MarketOrderDraft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = *qty,
        .metadata = OrderMetadata{.timeframe = std::string("1h"), .strategyTag = std::string("adapter-1")},
    }));
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(fallback.quantities, (std::vector<std::string>{"0.01", "0.01"}));
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open_v2(dbPath.string().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db.get(), "SELECT COUNT(*) FROM qlib_execution_slices WHERE status='filled';", -1, &stmt, nullptr), SQLITE_OK);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 2);
    std::error_code ec;
    fs::remove(dbPath, ec);
}

TEST(QlibExecutionPlannerTest, RevokesPendingSlicesWhenLatestDecisionContradicts) {
    const auto dbPath = uniqueDbPath("revoke");
    initializeDb(dbPath);
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open_v2(dbPath.string().c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> setupDb(raw, sqlite3_close);
    exec(setupDb.get(), "INSERT INTO qlib_strategy_runs(run_id, strategy_id, qlib_class, config_hash, model_id, model_run_id, interval, universe_hash, started_at_ms, completed_at_ms, status, error) VALUES('run-1','adapter-1','TopkDropoutStrategy','h','m','mr','1h','u',1,2,'succeeded',NULL);");
    exec(setupDb.get(), "INSERT INTO qlib_strategy_decisions(strategy_id, run_id, model_id, model_run_id, symbol, interval, asof_open_time_ms, generated_at_ms, action, direction, target_weight, score, score_percentile, confidence, reason) VALUES('adapter-1','run-1','m','mr','BTCUSDT','1h',1,2,'none','none',NULL,0,0,0,'reversed');");
    setupDb.reset();

    RecordingPlanner fallback;
    engine::QlibExecutionPlanner planner(dbPath.string(), fallback);
    boost::asio::io_context ioc;
    boost::asio::co_spawn(ioc, insertPlanWhenRequestAppears(dbPath.string(), 2), boost::asio::detached);
    auto qty = DecimalString::parse("0.02");
    ASSERT_TRUE(qty.has_value());
    auto result = runAwaitable(ioc, planner.executeMarket(MarketOrderDraft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = *qty,
        .metadata = OrderMetadata{.timeframe = std::string("1h"), .strategyTag = std::string("adapter-1")},
    }));
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(fallback.quantities.empty());
    ASSERT_EQ(sqlite3_open_v2(dbPath.string().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db.get(), "SELECT COUNT(*) FROM qlib_execution_slices WHERE status='revoked' AND revoke_reason LIKE 'direction_reversed%';", -1, &stmt, nullptr), SQLITE_OK);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 2);
    std::error_code ec;
    fs::remove(dbPath, ec);
}

TEST(QlibExecutionPlannerTest, RevokesSellSlicesWhenLatestDecisionFlipsLong) {
    const auto dbPath = uniqueDbPath("revoke_short");
    initializeDb(dbPath);
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open_v2(dbPath.string().c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> setupDb(raw, sqlite3_close);
    exec(setupDb.get(), "INSERT INTO qlib_strategy_runs(run_id, strategy_id, qlib_class, config_hash, model_id, model_run_id, interval, universe_hash, started_at_ms, completed_at_ms, status, error) VALUES('run-1','adapter-1','TopkDropoutStrategy','h','m','mr','1h','u',1,2,'succeeded',NULL);");
    exec(setupDb.get(), "INSERT INTO qlib_strategy_decisions(strategy_id, run_id, model_id, model_run_id, symbol, interval, asof_open_time_ms, generated_at_ms, action, direction, target_weight, score, score_percentile, confidence, reason) VALUES('adapter-1','run-1','m','mr','BTCUSDT','1h',1,2,'buy','long',NULL,0,0,0,'flipped');");
    setupDb.reset();

    RecordingPlanner fallback;
    engine::QlibExecutionPlanner planner(dbPath.string(), fallback);
    boost::asio::io_context ioc;
    boost::asio::co_spawn(ioc, insertPlanWhenRequestAppears(dbPath.string(), 2, "SELL"), boost::asio::detached);
    auto qty = DecimalString::parse("0.02");
    ASSERT_TRUE(qty.has_value());
    auto result = runAwaitable(ioc, planner.executeMarket(MarketOrderDraft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Sell,
        .quantity = *qty,
        .metadata = OrderMetadata{.timeframe = std::string("1h"), .strategyTag = std::string("adapter-1")},
    }));
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(fallback.quantities.empty());

    ASSERT_EQ(sqlite3_open_v2(dbPath.string().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            db.get(),
            "SELECT COUNT(*) FROM qlib_execution_slices WHERE status='revoked' AND revoke_reason LIKE 'direction_reversed%';",
            -1,
            &stmt,
            nullptr),
        SQLITE_OK);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 2);
    std::error_code ec;
    fs::remove(dbPath, ec);
}
