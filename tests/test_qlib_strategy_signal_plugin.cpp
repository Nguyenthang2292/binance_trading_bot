#include <gtest/gtest.h>

#include "catalog/plugin_handle.h"

#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#elif __has_include(<winsqlite/winsqlite3.h>)
#include <winsqlite/winsqlite3.h>
#else
#error "SQLite headers not found"
#endif

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* kPluginFilename = "strategy_qlib_strategy_signal.dll";

std::filesystem::path findPluginPathFrom(std::filesystem::path start) {
    if (start.empty()) {
        return {};
    }
    std::error_code ec;
    start = std::filesystem::absolute(start, ec);
    if (ec) {
        return {};
    }

    auto current = std::move(start);
    while (!current.empty()) {
        const auto candidate = current / "plugins" / kPluginFilename;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }

        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

std::filesystem::path pluginPath() {
    if (const auto byCwd = findPluginPathFrom(std::filesystem::current_path()); !byCwd.empty()) {
        return byCwd;
    }
    if (const auto byFile = findPluginPathFrom(std::filesystem::path(__FILE__).parent_path()); !byFile.empty()) {
        return byFile;
    }
    throw std::runtime_error("unable to locate strategy_qlib_strategy_signal.dll");
}

catalog::PluginHandle loadPlugin() {
    auto loaded = catalog::PluginHandle::load(pluginPath());
    if (!loaded) {
        throw std::runtime_error(loaded.error());
    }
    return std::move(*loaded);
}

using StrategyPtr = std::unique_ptr<strategy::IStrategy, catalog::PluginHandle::DestroyFn>;

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
    const auto path = base / ("qlib_strategy_signal_plugin_test_" + suffix);
    std::filesystem::create_directories(path);
    return path;
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
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

void seedValidDecisionDb(const std::string& dbPath) {
    sqlite3* rawDb = nullptr;
    ASSERT_EQ(sqlite3_open(dbPath.c_str(), &rawDb), SQLITE_OK);
    ASSERT_NE(rawDb, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);

    execOrThrow(db.get(), "PRAGMA user_version=7;");
    execOrThrow(
        db.get(),
        "CREATE TABLE qlib_adapter_runtime_state ("
        "adapter_id TEXT NOT NULL,"
        "interval TEXT NOT NULL,"
        "execution_mode TEXT NOT NULL,"
        "active_run_id TEXT,"
        "state_version INTEGER NOT NULL,"
        "PRIMARY KEY(adapter_id, interval));");
    execOrThrow(
        db.get(),
        "CREATE TABLE qlib_strategy_runs ("
        "run_id TEXT PRIMARY KEY,"
        "strategy_id TEXT NOT NULL,"
        "status TEXT NOT NULL,"
        "universe_hash TEXT NOT NULL);");
    execOrThrow(
        db.get(),
        "CREATE TABLE qlib_strategy_decisions ("
        "strategy_id TEXT NOT NULL,"
        "run_id TEXT NOT NULL,"
        "model_id TEXT,"
        "model_run_id TEXT,"
        "symbol TEXT NOT NULL,"
        "interval TEXT NOT NULL,"
        "asof_open_time_ms INTEGER NOT NULL,"
        "generated_at_ms INTEGER NOT NULL,"
        "action TEXT NOT NULL,"
        "direction TEXT NOT NULL,"
        "target_weight REAL,"
        "score REAL,"
        "score_percentile REAL,"
        "confidence REAL NOT NULL,"
        "reason TEXT);");
    execOrThrow(
        db.get(),
        "INSERT INTO qlib_adapter_runtime_state("
        "adapter_id, interval, execution_mode, active_run_id, state_version"
        ") VALUES('topk_dropout_30m_v1', '30m', 'live', 'run-1', 1);");
    execOrThrow(
        db.get(),
        "INSERT INTO qlib_strategy_runs(run_id, strategy_id, status, universe_hash) "
        "VALUES('run-1', 'topk_dropout_30m_v1', 'succeeded', 'test_universe');");

    const std::string decisionSql =
        "INSERT INTO qlib_strategy_decisions("
        "strategy_id, run_id, model_id, model_run_id, symbol, interval, "
        "asof_open_time_ms, generated_at_ms, action, direction, target_weight, "
        "score, score_percentile, confidence, reason"
        ") VALUES('topk_dropout_30m_v1', 'run-1', 'model-1', 'model-run-1', "
        "'BTCUSDT', '30m', " +
        std::to_string(nowMs()) + ", " + std::to_string(nowMs()) +
        ", 'buy', 'long', 1.0, 0.9, 0.95, 0.95, 'test decision');";
    execOrThrow(db.get(), decisionSql.c_str());
}

} // namespace

TEST(QlibStrategySignalPluginTest, CatalogCreationDoesNotRequireInitializedDb) {
    auto plugin = loadPlugin();
    const std::string config = R"json(
{
  "name": "Qlib TopK Dropout 30m Adapter",
  "type": "qlib_strategy_signal",
  "intervals": ["30m"],
  "params": {
    "source": "sqlite",
    "db_path": "data/qlib_smoke/uninitialized_for_catalog_test.db",
    "strategy_id": "topk_dropout_30m_v1",
    "universe_hash_strict": false
  }
}
)json";

    strategy::IStrategy* raw = plugin.create(config.c_str());
    ASSERT_NE(raw, nullptr);
    StrategyPtr strategy(raw, plugin.destroyFunction());

    EXPECT_EQ(strategy->config().type, "qlib_strategy_signal");
    EXPECT_EQ(strategy->config().adapterId, "topk_dropout_30m_v1");

    const auto signal = strategy->evaluate("BTCUSDT", "30m", std::vector<Kline>{});
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::None);
    EXPECT_NE(signal.reason.find("schema_unavailable"), std::string::npos);
}

TEST(QlibStrategySignalPluginTest, SuccessfulSchemaCheckIsCachedAcrossEvaluations) {
    const auto tempDir = makeTempDir();
    TempDirGuard guard{tempDir};
    const auto dbPath = (tempDir / "strategy.db").generic_string();
    seedValidDecisionDb(dbPath);

    auto plugin = loadPlugin();
    const std::string config = R"json(
{
  "name": "Qlib TopK Dropout 30m Adapter",
  "type": "qlib_strategy_signal",
  "intervals": ["30m"],
  "params": {
    "source": "sqlite",
    "db_path": ")json" + dbPath + R"json(",
    "strategy_id": "topk_dropout_30m_v1",
    "universe_hash_strict": false,
    "max_artifact_age_seconds": 999999999,
    "max_data_age_seconds": 999999999
  }
}
)json";

    strategy::IStrategy* raw = plugin.create(config.c_str());
    ASSERT_NE(raw, nullptr);
    StrategyPtr strategy(raw, plugin.destroyFunction());

    const auto first = strategy->evaluate("BTCUSDT", "30m", std::vector<Kline>{});
    ASSERT_EQ(first.direction, strategy::Signal::Direction::Long);

    sqlite3* rawDb = nullptr;
    ASSERT_EQ(sqlite3_open(dbPath.c_str(), &rawDb), SQLITE_OK);
    ASSERT_NE(rawDb, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);
    execOrThrow(db.get(), "PRAGMA user_version=6;");

    const auto second = strategy->evaluate("BTCUSDT", "30m", std::vector<Kline>{});
    EXPECT_EQ(second.direction, strategy::Signal::Direction::Long);
    EXPECT_EQ(second.reason.find("schema_unavailable"), std::string::npos);
}
