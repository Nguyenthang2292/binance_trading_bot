#include <gtest/gtest.h>

#include "orchestration/model_publisher.h"
#include "orchestration/qlib_state_store.h"

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
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

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
    const auto path = base / ("model_publisher_test_" + suffix);
    std::filesystem::create_directories(path);
    return path;
}

void writeText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << text;
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

void createStagingRun(const std::filesystem::path& stagingDir, std::string_view runId) {
    const auto runDir = stagingDir / runId;
    writeText(runDir / "model.txt", "model-weights");
    writeText(
        runDir / "report.json",
        R"({"oos_metrics":{"ic":0.1,"rank_ic":0.2,"oos_rows":42},"feature_columns":["close","volume"]})");
}

orchestration::ModelPublishRequest makeRequest(
    const std::filesystem::path& root,
    const std::string& dbPath,
    std::string runId = "run_1") {
    orchestration::ModelPublishRequest request;
    request.dbPath = dbPath;
    request.modelId = "lightgbm_1h_v1";
    request.interval = "1h";
    request.runId = std::move(runId);
    request.horizonBars = 4;
    request.stagingDir = (root / "staging").string();
    request.artifactsDir = (root / "artifacts").string();
    request.manifestPath = (root / "current" / "lightgbm_1h_v1.json").string();
    return request;
}

void initializeRuntimeState(const std::string& dbPath) {
    orchestration::QlibStateStoreConfig cfg;
    cfg.dbPath = dbPath;
    cfg.modelId = "lightgbm_1h_v1";
    cfg.interval = "1h";
    const auto stateStore = orchestration::QlibStateStore::create(cfg);
    stateStore->initializeSchema();
    stateStore->initializeRuntimeStateIfMissing();
}

void initializeRuntimeStateOnly(const std::string& dbPath) {
    sqlite3* rawDb = nullptr;
    ASSERT_EQ(sqlite3_open(dbPath.c_str(), &rawDb), SQLITE_OK);
    ASSERT_NE(rawDb, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);
    execOrThrow(
        db.get(),
        "CREATE TABLE qlib_runtime_state ("
        "model_id TEXT NOT NULL,"
        "interval TEXT NOT NULL,"
        "execution_mode TEXT NOT NULL,"
        "active_run_id TEXT,"
        "active_manifest_path TEXT,"
        "state_version INTEGER NOT NULL DEFAULT 0,"
        "promoted_at_ms INTEGER,"
        "rollback_reason TEXT,"
        "updated_at_ms INTEGER NOT NULL,"
        "PRIMARY KEY (model_id, interval)"
        ");");
    execOrThrow(
        db.get(),
        "INSERT INTO qlib_runtime_state(model_id, interval, execution_mode, updated_at_ms) "
        "VALUES('lightgbm_1h_v1', '1h', 'shadow', 1);");
}

} // namespace

TEST(ModelPublisherTest, PublishUpdatesDbManifestAndRemovesStaging) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};
    const std::string dbPath = (tmp / "predictions.db").string();
    initializeRuntimeState(dbPath);

    auto request = makeRequest(tmp, dbPath);
    createStagingRun(request.stagingDir, request.runId);

    std::string error;
    ASSERT_TRUE(orchestration::ModelPublisher::publish(request, error)) << error;

    const auto artifactRunDir = tmp / "artifacts" / request.modelId / request.runId;
    EXPECT_TRUE(std::filesystem::exists(artifactRunDir / "model.txt"));
    EXPECT_TRUE(std::filesystem::exists(artifactRunDir / "report.json"));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(request.stagingDir) / request.runId));

    std::ifstream manifestInput(request.manifestPath);
    const auto manifest = nlohmann::json::parse(manifestInput);
    EXPECT_EQ(manifest.at("run_id").get<std::string>(), request.runId);
    EXPECT_EQ(manifest.at("model_path").get<std::string>(), (artifactRunDir / "model.txt").string());

    EXPECT_EQ(
        scalarText(dbPath, "SELECT active_run_id FROM qlib_runtime_state WHERE model_id='lightgbm_1h_v1';"),
        request.runId);
    EXPECT_EQ(
        scalarText(dbPath, "SELECT status FROM qlib_model_runs WHERE run_id='run_1';"),
        "active");
}

TEST(ModelPublisherTest, PublishFailureLeavesStagingAndNoVisibleManifest) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};
    const std::string dbPath = (tmp / "predictions.db").string();
    initializeRuntimeStateOnly(dbPath);

    auto request = makeRequest(tmp, dbPath, "run_missing_model_runs");
    createStagingRun(request.stagingDir, request.runId);

    std::string error;
    EXPECT_FALSE(orchestration::ModelPublisher::publish(request, error));
    EXPECT_NE(error.find("qlib_model_runs"), std::string::npos);

    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(request.stagingDir) / request.runId / "model.txt"));
    EXPECT_FALSE(std::filesystem::exists(tmp / "artifacts" / request.modelId / request.runId));
    EXPECT_FALSE(std::filesystem::exists(request.manifestPath));
}
