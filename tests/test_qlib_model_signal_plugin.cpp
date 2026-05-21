#include <gtest/gtest.h>

#include "catalog/plugin_handle.h"

#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#elif __has_include(<winsqlite/winsqlite3.h>)
#include <winsqlite/winsqlite3.h>
#else
#error "SQLite headers not found"
#endif

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kPluginFilename = "strategy_qlib_model_signal.dll";

struct TempDirGuard {
    std::filesystem::path path;
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::filesystem::path makeTempDir() {
    std::error_code ec;
    const auto base = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return {};
    }
    const auto suffix = std::to_string(
        static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = base / ("qlib_model_signal_test_" + suffix);
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return {};
    }
    return path;
}

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
        const std::vector<std::filesystem::path> candidates{
            current / "bin" / "Debug" / "plugins" / kPluginFilename,
            current / "bin" / "Release" / "plugins" / kPluginFilename,
            current / "build" / "bin" / "Debug" / "plugins" / kPluginFilename,
            current / "build" / "bin" / "Release" / "plugins" / kPluginFilename,
            current / "build" / "windows-msvc-debug" / "bin" / "Debug" / "plugins" / kPluginFilename,
            current / "plugins" / kPluginFilename,
        };
        for (const auto& candidate : candidates) {
            if (std::filesystem::exists(candidate, ec) && !ec) {
                return candidate;
            }
            ec.clear();
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
    throw std::runtime_error("unable to locate strategy_qlib_model_signal.dll");
}

catalog::PluginHandle loadPlugin() {
    auto loaded = catalog::PluginHandle::load(pluginPath());
    if (!loaded) {
        throw std::runtime_error(loaded.error());
    }
    return std::move(*loaded);
}

using StrategyPtr = std::unique_ptr<strategy::IStrategy, catalog::PluginHandle::DestroyFn>;

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
    std::string message = err ? err : "sqlite error";
    if (err) {
        sqlite3_free(err);
    }
    throw std::runtime_error(message);
}

void createPredictionSchema(const std::filesystem::path& dbPath) {
    sqlite3* rawDb = nullptr;
    ASSERT_EQ(sqlite3_open(dbPath.string().c_str(), &rawDb), SQLITE_OK);
    ASSERT_NE(rawDb, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);

    execOrThrow(
        db.get(),
        "CREATE TABLE IF NOT EXISTS qlib_predictions ("
        "model_id TEXT NOT NULL,"
        "symbol TEXT NOT NULL,"
        "interval TEXT NOT NULL,"
        "asof_open_time_ms INTEGER NOT NULL,"
        "generated_at_ms INTEGER NOT NULL,"
        "score REAL NOT NULL,"
        "rank INTEGER,"
        "score_percentile REAL,"
        "PRIMARY KEY (model_id, symbol, interval, asof_open_time_ms)"
        ");");
    execOrThrow(
        db.get(),
        "CREATE INDEX IF NOT EXISTS idx_qlib_pred_lookup "
        "ON qlib_predictions (model_id, interval, generated_at_ms DESC);");
}

void upsertPrediction(
    const std::filesystem::path& dbPath,
    std::string_view modelId,
    std::string_view symbol,
    std::string_view interval,
    int64_t asofOpenTimeMs,
    int64_t generatedAtMs,
    double score,
    int rank,
    double scorePercentile) {
    sqlite3* rawDb = nullptr;
    ASSERT_EQ(sqlite3_open(dbPath.string().c_str(), &rawDb), SQLITE_OK);
    ASSERT_NE(rawDb, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);

    const char* sql =
        "INSERT INTO qlib_predictions(model_id, symbol, interval, asof_open_time_ms, generated_at_ms, score, rank, score_percentile) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(model_id, symbol, interval, asof_open_time_ms) DO UPDATE SET "
        "generated_at_ms=excluded.generated_at_ms, score=excluded.score, rank=excluded.rank, score_percentile=excluded.score_percentile;";
    sqlite3_stmt* rawStmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db.get(), sql, -1, &rawStmt, nullptr), SQLITE_OK);
    ASSERT_NE(rawStmt, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);

    sqlite3_bind_text(stmt.get(), 1, modelId.data(), static_cast<int>(modelId.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, symbol.data(), static_cast<int>(symbol.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, interval.data(), static_cast<int>(interval.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 4, asofOpenTimeMs);
    sqlite3_bind_int64(stmt.get(), 5, generatedAtMs);
    sqlite3_bind_double(stmt.get(), 6, score);
    sqlite3_bind_int(stmt.get(), 7, rank);
    sqlite3_bind_double(stmt.get(), 8, scorePercentile);

    ASSERT_EQ(sqlite3_step(stmt.get()), SQLITE_DONE);
}

std::string makeConfigJson(
    const std::filesystem::path& dbPath,
    std::string_view failMode,
    bool dryRun,
    std::string_view confidenceMode = "rank",
    double minConfidencePercentile = 0.6,
    int maxArtifactAgeSeconds = 7200,
    int maxDataAgeSeconds = 3600) {
    nlohmann::json cfg = {
        {"name", "Qlib LightGBM 1h Signal"},
        {"type", "qlib_model_signal"},
        {"intervals", {"1h"}},
        {"scan_interval_seconds", 900},
        {"max_hold_duration_seconds", 86400},
        {"risk_pct", 0.01},
        {"sl_multiplier", 1.5},
        {"tp_multiplier", 3.0},
        {"takeProfitPercent", 20.0},
        {"min_notional", 1.0},
        {"atr_period", 14},
        {"min_confidence", 0.0},
        {"params",
         {
             {"source", "sqlite"},
             {"db_path", dbPath.string()},
             {"model_id", "lightgbm_1h_v1"},
             {"max_artifact_age_seconds", maxArtifactAgeSeconds},
             {"max_data_age_seconds", maxDataAgeSeconds},
             {"long_threshold", 0.003},
             {"short_threshold", -0.003},
             {"confidence_mode", confidenceMode},
             {"min_confidence_percentile", minConfidencePercentile},
             {"dry_run", dryRun},
             {"fail_mode", failMode},
             {"score_to_confidence_scale", 0.01},
         }},
    };
    return cfg.dump();
}

StrategyPtr createStrategy(catalog::PluginHandle& plugin, const std::string& configJson) {
    strategy::IStrategy* raw = plugin.create(configJson.c_str());
    if (!raw) {
        throw std::runtime_error("createStrategy returned nullptr");
    }
    return StrategyPtr(raw, plugin.destroyFunction());
}

} // namespace

TEST(QlibModelSignalPluginTest, MissingDbReturnsNoneForBothFailModes) {
    auto plugin = loadPlugin();
    const auto tmp = makeTempDir();
    ASSERT_FALSE(tmp.empty());
    TempDirGuard guard{tmp};
    const auto dbPath = tmp / "missing.db";

    auto openStrategy = createStrategy(plugin, makeConfigJson(dbPath, "open", false));
    auto openSignal = openStrategy->evaluate("BTCUSDT", "1h", {});
    EXPECT_EQ(openSignal.direction, strategy::Signal::Direction::None);

    auto closedStrategy = createStrategy(plugin, makeConfigJson(dbPath, "closed", false));
    auto closedSignal = closedStrategy->evaluate("BTCUSDT", "1h", {});
    EXPECT_EQ(closedSignal.direction, strategy::Signal::Direction::None);
}

TEST(QlibModelSignalPluginTest, StaleArtifactReturnsNone) {
    auto plugin = loadPlugin();
    const auto tmp = makeTempDir();
    ASSERT_FALSE(tmp.empty());
    TempDirGuard guard{tmp};
    const auto dbPath = tmp / "predictions.db";

    createPredictionSchema(dbPath);
    const int64_t now = nowMs();
    upsertPrediction(
        dbPath,
        "lightgbm_1h_v1",
        "BTCUSDT",
        "1h",
        now - 1800 * 1000,
        now - 8000 * 1000,
        0.01,
        1,
        0.97);

    auto strategy = createStrategy(plugin, makeConfigJson(dbPath, "open", false, "rank", 0.6, 3600, 3600));
    auto signal = strategy->evaluate("BTCUSDT", "1h", {});
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::None);
}

TEST(QlibModelSignalPluginTest, StaleDataReturnsNone) {
    auto plugin = loadPlugin();
    const auto tmp = makeTempDir();
    ASSERT_FALSE(tmp.empty());
    TempDirGuard guard{tmp};
    const auto dbPath = tmp / "predictions.db";

    createPredictionSchema(dbPath);
    const int64_t now = nowMs();
    upsertPrediction(
        dbPath,
        "lightgbm_1h_v1",
        "BTCUSDT",
        "1h",
        now - 7200 * 1000,
        now - 60 * 1000,
        0.01,
        1,
        0.97);

    auto strategy = createStrategy(plugin, makeConfigJson(dbPath, "open", false, "rank", 0.6, 3600, 3600));
    auto signal = strategy->evaluate("BTCUSDT", "1h", {});
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::None);
}

TEST(QlibModelSignalPluginTest, DryRunKeepsCandidateDirectionAndAnnotatesReason) {
    auto plugin = loadPlugin();
    const auto tmp = makeTempDir();
    ASSERT_FALSE(tmp.empty());
    TempDirGuard guard{tmp};
    const auto dbPath = tmp / "predictions.db";

    createPredictionSchema(dbPath);
    const int64_t now = nowMs();
    upsertPrediction(
        dbPath,
        "lightgbm_1h_v1",
        "BTCUSDT",
        "1h",
        now - 900 * 1000,
        now - 60 * 1000,
        0.02,
        1,
        0.99);

    auto strategy = createStrategy(plugin, makeConfigJson(dbPath, "open", true));
    auto signal = strategy->evaluate("BTCUSDT", "1h", {});
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::Long);
    EXPECT_NE(signal.reason.find("dry_run=true"), std::string::npos);
    EXPECT_NE(signal.reason.find("would_be=Long"), std::string::npos);
}

TEST(QlibModelSignalPluginTest, RankConfidenceUsesScorePercentile) {
    auto plugin = loadPlugin();
    const auto tmp = makeTempDir();
    ASSERT_FALSE(tmp.empty());
    TempDirGuard guard{tmp};
    const auto dbPath = tmp / "predictions.db";

    createPredictionSchema(dbPath);
    const int64_t now = nowMs();
    upsertPrediction(
        dbPath,
        "lightgbm_1h_v1",
        "BTCUSDT",
        "1h",
        now - 900 * 1000,
        now - 60 * 1000,
        0.02,
        1,
        0.97);

    auto strategy = createStrategy(plugin, makeConfigJson(dbPath, "open", false, "rank", 0.6));
    auto signal = strategy->evaluate("BTCUSDT", "1h", {});
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::Long);
    EXPECT_DOUBLE_EQ(signal.confidence, 0.97);
}

TEST(QlibModelSignalPluginTest, MinConfidencePercentileFiltersLongAndShort) {
    auto plugin = loadPlugin();
    const auto tmp = makeTempDir();
    ASSERT_FALSE(tmp.empty());
    TempDirGuard guard{tmp};
    const auto dbPath = tmp / "predictions.db";

    createPredictionSchema(dbPath);
    const int64_t now = nowMs();
    upsertPrediction(
        dbPath,
        "lightgbm_1h_v1",
        "BTCUSDT",
        "1h",
        now - 900 * 1000,
        now - 60 * 1000,
        0.01,
        1,
        0.55);

    auto strategy = createStrategy(plugin, makeConfigJson(dbPath, "open", false, "rank", 0.6));
    auto blockedLong = strategy->evaluate("BTCUSDT", "1h", {});
    EXPECT_EQ(blockedLong.direction, strategy::Signal::Direction::None);

    upsertPrediction(
        dbPath,
        "lightgbm_1h_v1",
        "BTCUSDT",
        "1h",
        now - 900 * 1000,
        now - 60 * 1000,
        -0.02,
        20,
        0.55);
    auto blockedShort = strategy->evaluate("BTCUSDT", "1h", {});
    EXPECT_EQ(blockedShort.direction, strategy::Signal::Direction::None);

    upsertPrediction(
        dbPath,
        "lightgbm_1h_v1",
        "BTCUSDT",
        "1h",
        now - 900 * 1000,
        now - 60 * 1000,
        -0.02,
        20,
        0.20);
    auto allowedShort = strategy->evaluate("BTCUSDT", "1h", {});
    EXPECT_EQ(allowedShort.direction, strategy::Signal::Direction::Short);
}
