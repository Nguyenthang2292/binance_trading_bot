#include "orchestration/model_publisher.h"
#include "orchestration/sqlite_helpers.h"

#include <nlohmann/json.hpp>
#include <openssl/err.h>
#include <openssl/evp.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace orchestration {

namespace {

using sqlite_helpers::bindText;
using sqlite_helpers::execSql;
using sqlite_helpers::nowMs;
using sqlite_helpers::sqliteError;

std::string opensslError() {
    const unsigned long code = ERR_get_error();
    if (code == 0) {
        return "unknown OpenSSL error";
    }
    std::array<char, 256> buffer{};
    ERR_error_string_n(code, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

std::string digestToHex(const unsigned char* data, unsigned int len) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < len; ++i) {
        out << std::setw(2) << static_cast<unsigned int>(data[i]);
    }
    return out.str();
}

std::string sha256Bytes(std::string_view value) {
    EVP_MD_CTX* rawCtx = EVP_MD_CTX_new();
    if (!rawCtx) {
        throw std::runtime_error("failed to allocate sha256 context");
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(rawCtx, &EVP_MD_CTX_free);
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx.get(), value.data(), value.size()) != 1) {
        throw std::runtime_error("failed to compute sha256: " + opensslError());
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLen = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &digestLen) != 1) {
        throw std::runtime_error("failed to finalize sha256: " + opensslError());
    }
    return digestToHex(digest.data(), digestLen);
}

std::string sha256File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open file for sha256: " + path.string());
    }
    EVP_MD_CTX* rawCtx = EVP_MD_CTX_new();
    if (!rawCtx) {
        throw std::runtime_error("failed to allocate sha256 context");
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(rawCtx, &EVP_MD_CTX_free);
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("failed to initialize sha256: " + opensslError());
    }
    std::array<char, 8192> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytesRead = input.gcount();
        if (bytesRead > 0 &&
            EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<size_t>(bytesRead)) != 1) {
            throw std::runtime_error("failed to update sha256: " + opensslError());
        }
    }
    if (input.bad() || (input.fail() && !input.eof())) {
        throw std::runtime_error("failed while reading file for sha256: " + path.string());
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLen = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &digestLen) != 1) {
        throw std::runtime_error("failed to finalize sha256: " + opensslError());
    }
    return digestToHex(digest.data(), digestLen);
}

bool ensureRuntimeStateExists(
    sqlite3* db,
    const std::string& modelId,
    const std::string& interval,
    std::string& error) {
    const char* sql =
        "SELECT 1 FROM qlib_runtime_state WHERE model_id = ? AND interval = ? LIMIT 1;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        error = sqliteError(db, "prepare runtime state preflight failed");
        return false;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    bindText(stmt.get(), 1, modelId);
    bindText(stmt.get(), 2, interval);
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        error = "qlib_runtime_state row not found for model_id='" + modelId +
            "' interval='" + interval + "'; check state-store and publish config use the same pair";
        return false;
    }
    error = sqliteError(db, "runtime state preflight failed");
    return false;
}

bool copyRunDirectory(
    const std::filesystem::path& runStaging,
    const std::filesystem::path& artifactRunDir,
    std::string& error) {
    std::error_code ec;
    if (std::filesystem::exists(artifactRunDir, ec)) {
        error = "artifact run dir already exists: " + artifactRunDir.string();
        return false;
    }
    ec.clear();
    std::filesystem::copy(
        runStaging,
        artifactRunDir,
        std::filesystem::copy_options::recursive,
        ec);
    if (ec) {
        error = "copy staging failed: " + ec.message();
        return false;
    }
    return true;
}

bool replaceFileAtomically(
    const std::filesystem::path& source,
    const std::filesystem::path& target,
    std::string& error) {
#if defined(_WIN32)
    if (!MoveFileExW(
            source.wstring().c_str(),
            target.wstring().c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "atomic file swap failed";
        return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(source, target, ec);
    if (ec) {
        error = "atomic file swap failed: " + ec.message();
        return false;
    }
#endif
    return true;
}

void removeBestEffort(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

} // namespace

bool ModelPublisher::publish(const ModelPublishRequest& request, std::string& error) {
    try {
        const std::filesystem::path stagingRoot(request.stagingDir);
        const std::filesystem::path runStaging = stagingRoot / request.runId;
        const std::filesystem::path modelPath = runStaging / "model.txt";
        const std::filesystem::path reportPath = runStaging / "report.json";
        if (!std::filesystem::exists(modelPath)) {
            error = "missing model.txt in staging run dir";
            return false;
        }
        if (!std::filesystem::exists(reportPath)) {
            error = "missing report.json in staging run dir";
            return false;
        }

        sqlite3* rawDb = nullptr;
        if (sqlite3_open(request.dbPath.c_str(), &rawDb) != SQLITE_OK || rawDb == nullptr) {
            if (rawDb) {
                sqlite3_close(rawDb);
            }
            error = "failed to open sqlite db for model publish";
            return false;
        }
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);

        std::string sqlError;
        if (!execSql(db.get(), "PRAGMA journal_mode=WAL;", sqlError)) {
            error = sqlError;
            return false;
        }
        if (!execSql(db.get(), "PRAGMA busy_timeout=5000;", sqlError)) {
            error = sqlError;
            return false;
        }
        if (!ensureRuntimeStateExists(db.get(), request.modelId, request.interval, error)) {
            return false;
        }

        const std::filesystem::path artifactRunDir =
            std::filesystem::path(request.artifactsDir) / request.modelId / request.runId;
        const std::filesystem::path publishedModelPath = artifactRunDir / "model.txt";
        const std::filesystem::path publishedReportPath = artifactRunDir / "report.json";

        std::string reportRaw;
        {
            std::ifstream in(reportPath);
            reportRaw.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const auto report = nlohmann::json::parse(reportRaw.empty() ? "{}" : reportRaw);
        const auto oos = report.value("oos_metrics", nlohmann::json::object());
        const double oosIc = oos.value("ic", 0.0);
        const double oosRankIc = oos.value("rank_ic", 0.0);
        const int oosRows = oos.value("oos_rows", 0);
        const std::string modelHash = sha256File(modelPath);
        const std::string featureSchemaHash = report.contains("feature_columns")
            ? sha256Bytes(report.at("feature_columns").dump())
            : modelHash;
        const std::string datasetFingerprint = modelHash;

        const int64_t publishedAt = nowMs();
        nlohmann::json manifest = {
            {"model_id", request.modelId},
            {"run_id", request.runId},
            {"interval", request.interval},
            {"horizon_bars", request.horizonBars},
            {"model_path", publishedModelPath.string()},
            {"report_path", publishedReportPath.string()},
            {"feature_schema_hash", featureSchemaHash},
            {"dataset_fingerprint", datasetFingerprint},
            {"published_at_ms", publishedAt},
        };

        const std::filesystem::path manifestPath(request.manifestPath);
        std::filesystem::create_directories(manifestPath.parent_path());
        const std::filesystem::path tmpPath = manifestPath.string() + ".tmp";
        {
            std::ofstream out(tmpPath, std::ios::trunc);
            out << manifest.dump(2);
            out.flush();
            if (!out) {
                error = "failed to write manifest tmp file (disk full?)";
                return false;
            }
        }

        if (!execSql(db.get(), "BEGIN IMMEDIATE TRANSACTION;", sqlError)) {
            removeBestEffort(tmpPath);
            error = sqlError;
            return false;
        }

        auto rollbackDb = [&]() {
            std::string unused;
            (void)execSql(db.get(), "ROLLBACK;", unused);
        };
        bool copiedArtifact = false;
        bool swappedManifest = false;
        bool hadPreviousManifest = false;
        const std::filesystem::path manifestBackupPath = manifestPath.string() + ".bak_publish";
        auto rollbackFilesystem = [&]() {
            if (copiedArtifact) {
                removeBestEffort(artifactRunDir);
            }
            if (swappedManifest) {
                if (hadPreviousManifest) {
                    std::string restoreError;
                    (void)replaceFileAtomically(manifestBackupPath, manifestPath, restoreError);
                } else {
                    removeBestEffort(manifestPath);
                }
            } else {
                removeBestEffort(tmpPath);
                removeBestEffort(manifestBackupPath);
            }
        };

        const char* upsertRunSql =
            "INSERT INTO qlib_model_runs("
            "run_id, model_id, interval, horizon_bars, model_path, manifest_path, report_path, "
            "feature_schema_hash, dataset_fingerprint, oos_ic, oos_rank_ic, oos_rows, trained_at_ms, published_at_ms, status"
            ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'active') "
            "ON CONFLICT(run_id) DO UPDATE SET "
            "model_path=excluded.model_path,"
            "manifest_path=excluded.manifest_path,"
            "report_path=excluded.report_path,"
            "oos_ic=excluded.oos_ic,"
            "oos_rank_ic=excluded.oos_rank_ic,"
            "oos_rows=excluded.oos_rows,"
            "published_at_ms=excluded.published_at_ms,"
            "status='active';";
        sqlite3_stmt* runRaw = nullptr;
        if (sqlite3_prepare_v2(db.get(), upsertRunSql, -1, &runRaw, nullptr) != SQLITE_OK || runRaw == nullptr) {
            rollbackDb();
            rollbackFilesystem();
            error = sqliteError(db.get(), "prepare qlib_model_runs upsert failed");
            return false;
        }
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> runStmt(runRaw, sqlite3_finalize);
        bindText(runStmt.get(), 1, request.runId);
        bindText(runStmt.get(), 2, request.modelId);
        bindText(runStmt.get(), 3, request.interval);
        sqlite3_bind_int(runStmt.get(), 4, request.horizonBars);
        bindText(runStmt.get(), 5, publishedModelPath.string());
        bindText(runStmt.get(), 6, manifestPath.string());
        bindText(runStmt.get(), 7, publishedReportPath.string());
        bindText(runStmt.get(), 8, featureSchemaHash);
        bindText(runStmt.get(), 9, datasetFingerprint);
        sqlite3_bind_double(runStmt.get(), 10, oosIc);
        sqlite3_bind_double(runStmt.get(), 11, oosRankIc);
        sqlite3_bind_int(runStmt.get(), 12, oosRows);
        sqlite3_bind_int64(runStmt.get(), 13, publishedAt);
        sqlite3_bind_int64(runStmt.get(), 14, publishedAt);
        if (sqlite3_step(runStmt.get()) != SQLITE_DONE) {
            rollbackDb();
            rollbackFilesystem();
            error = sqliteError(db.get(), "upsert qlib_model_runs failed");
            return false;
        }

        const char* updateStateSql =
            "UPDATE qlib_runtime_state "
            "SET active_run_id=?, active_manifest_path=?, state_version=state_version+1, updated_at_ms=? "
            "WHERE model_id=? AND interval=?;";
        sqlite3_stmt* stateRaw = nullptr;
        if (sqlite3_prepare_v2(db.get(), updateStateSql, -1, &stateRaw, nullptr) != SQLITE_OK || stateRaw == nullptr) {
            rollbackDb();
            rollbackFilesystem();
            error = sqliteError(db.get(), "prepare runtime state update failed");
            return false;
        }
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stateStmt(stateRaw, sqlite3_finalize);
        bindText(stateStmt.get(), 1, request.runId);
        bindText(stateStmt.get(), 2, manifestPath.string());
        sqlite3_bind_int64(stateStmt.get(), 3, publishedAt);
        bindText(stateStmt.get(), 4, request.modelId);
        bindText(stateStmt.get(), 5, request.interval);
        if (sqlite3_step(stateStmt.get()) != SQLITE_DONE) {
            rollbackDb();
            rollbackFilesystem();
            error = sqliteError(db.get(), "update runtime state failed");
            return false;
        }
        if (sqlite3_changes(db.get()) == 0) {
            rollbackDb();
            rollbackFilesystem();
            error = "qlib_runtime_state row not found for model_id='" + request.modelId +
                "' interval='" + request.interval + "'; check state-store and publish config use the same pair";
            return false;
        }
        stateStmt.reset();
        runStmt.reset();

        std::error_code artifactEc;
        std::filesystem::create_directories(artifactRunDir.parent_path(), artifactEc);
        if (artifactEc) {
            rollbackDb();
            rollbackFilesystem();
            error = "artifact dir create failed: " + artifactEc.message();
            return false;
        }
        if (std::filesystem::exists(artifactRunDir, artifactEc)) {
            rollbackDb();
            rollbackFilesystem();
            error = "artifact run dir already exists: " + artifactRunDir.string();
            return false;
        }
        if (artifactEc) {
            rollbackDb();
            rollbackFilesystem();
            error = "artifact run dir preflight failed: " + artifactEc.message();
            return false;
        }
        copiedArtifact = true;
        if (!copyRunDirectory(runStaging, artifactRunDir, error)) {
            rollbackDb();
            rollbackFilesystem();
            return false;
        }

        removeBestEffort(manifestBackupPath);
        std::error_code ec;
        hadPreviousManifest = std::filesystem::exists(manifestPath, ec);
        if (ec) {
            rollbackDb();
            rollbackFilesystem();
            error = "manifest preflight failed: " + ec.message();
            return false;
        }
        if (hadPreviousManifest) {
            std::filesystem::copy_file(
                manifestPath,
                manifestBackupPath,
                std::filesystem::copy_options::overwrite_existing,
                ec);
            if (ec) {
                rollbackDb();
                rollbackFilesystem();
                error = "manifest backup failed: " + ec.message();
                return false;
            }
        }
        if (!replaceFileAtomically(tmpPath, manifestPath, error)) {
            rollbackDb();
            rollbackFilesystem();
            return false;
        }
        swappedManifest = true;

        if (!execSql(db.get(), "COMMIT;", sqlError)) {
            rollbackDb();
            rollbackFilesystem();
            error = sqlError;
            return false;
        }
        removeBestEffort(manifestBackupPath);
        removeBestEffort(runStaging);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace orchestration
