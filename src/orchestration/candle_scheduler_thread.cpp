#include "orchestration/candle_scheduler_thread.h"

#include "logger.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#elif __has_include(<winsqlite/winsqlite3.h>)
#include <winsqlite/winsqlite3.h>
#else
#error "SQLite headers not found"
#endif

namespace orchestration {

namespace {

std::optional<std::string> loadPublishedModelPath(std::string_view dbPath, std::string_view runId) {
    if (dbPath.empty() || runId.empty()) {
        return std::nullopt;
    }

    sqlite3* rawDb = nullptr;
    if (sqlite3_open(std::string(dbPath).c_str(), &rawDb) != SQLITE_OK || rawDb == nullptr) {
        if (rawDb) {
            sqlite3_close(rawDb);
        }
        return std::nullopt;
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);

    const char* sql =
        "SELECT model_path, status "
        "FROM qlib_model_runs "
        "WHERE run_id = ? "
        "LIMIT 1;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        if (rawStmt) {
            sqlite3_finalize(rawStmt);
        }
        return std::nullopt;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    sqlite3_bind_text(stmt.get(), 1, runId.data(), static_cast<int>(runId.size()), SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW) {
        return std::nullopt;
    }

    const auto* modelPathText = sqlite3_column_text(stmt.get(), 0);
    const auto* statusText = sqlite3_column_text(stmt.get(), 1);
    const std::string modelPath = modelPathText ? reinterpret_cast<const char*>(modelPathText) : "";
    const std::string status = statusText ? reinterpret_cast<const char*>(statusText) : "";
    if (status != "active" || modelPath.empty()) {
        return std::nullopt;
    }
    return modelPath;
}

bool parseLeadingInt64(std::string_view text, int64_t* out) {
    if (!out) {
        return false;
    }
    const auto comma = text.find(',');
    const std::string_view token = comma == std::string_view::npos ? text : text.substr(0, comma);
    if (token.empty()) {
        return false;
    }
    int64_t value = 0;
    const auto* begin = token.data();
    const auto* end = token.data() + token.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return false;
    }
    *out = value;
    return true;
}

} // namespace

CandleSchedulerThread::CandleSchedulerThread(
    CandleSchedulerConfig config,
    IProcessRunner& runner,
    QlibStateStore& stateStore,
    PromotionChecker& promoter)
    : m_config(std::move(config)),
      m_runner(runner),
      m_stateStore(stateStore),
      m_promoter(promoter) {
    m_config.postCandleDelaySeconds = std::max(0, m_config.postCandleDelaySeconds);
}

void CandleSchedulerThread::notifyCandleClose(int64_t candleOpenTimeMs, std::string_view symbol) {
    (void)symbol;
    if (candleOpenTimeMs <= 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (candleOpenTimeMs > m_pendingCandleMs) {
            m_pendingCandleMs = candleOpenTimeMs;
        }
    }
    m_cv.notify_one();
}

std::vector<std::string> CandleSchedulerThread::buildPhase3Cmd(int64_t asofMs) const {
    return buildPhase3Cmd(
        asofMs,
        (std::filesystem::path(m_config.modelDir) / ("lightgbm_" + m_config.interval + ".txt")).string());
}

std::vector<std::string> CandleSchedulerThread::buildPhase3Cmd(int64_t asofMs, std::string_view modelPath) const {
    std::vector<std::string> cmd;
    cmd.reserve(24);
    cmd.push_back(m_config.pythonExe);
    cmd.push_back((std::filesystem::path(m_config.scriptsDir) / "predict_latest.py").string());
    cmd.push_back("--dataset");
    cmd.push_back((std::filesystem::path(m_config.dataDir) / ("klines_" + m_config.interval + ".csv")).string());
    cmd.push_back("--model-path");
    cmd.push_back(std::string(modelPath));
    cmd.push_back("--model-id");
    cmd.push_back(m_config.modelId);
    cmd.push_back("--interval");
    cmd.push_back(m_config.interval);
    cmd.push_back("--db-path");
    cmd.push_back(m_config.dbPath);
    cmd.push_back("--asof-ms");
    cmd.push_back(std::to_string(asofMs));
    cmd.push_back("--ready-dir");
    cmd.push_back(m_config.readyDir);
    cmd.push_back("--debug-json");
    cmd.push_back((std::filesystem::path(m_config.dataDir) / "debug_latest.json").string());
    return cmd;
}

std::optional<std::string> CandleSchedulerThread::resolvePublishedModelPath() const {
    const auto snapshot = m_stateStore.snapshot();
    if (!snapshot.available || snapshot.activeRunId.empty()) {
        return std::nullopt;
    }
    auto modelPath = loadPublishedModelPath(m_stateStore.dbPath(), snapshot.activeRunId);
    if (!modelPath || modelPath->empty()) {
        return std::nullopt;
    }
    if (!std::filesystem::exists(*modelPath)) {
        return std::nullopt;
    }
    return modelPath;
}

bool CandleSchedulerThread::datasetContainsAsofMs(int64_t asofMs) const {
    if (asofMs <= 0) {
        return false;
    }
    const std::filesystem::path datasetPath =
        std::filesystem::path(m_config.dataDir) / ("klines_" + m_config.interval + ".csv");
    std::ifstream input(datasetPath, std::ios::binary);
    if (!input) {
        return false;
    }

    std::string line;
    while (std::getline(input, line)) {
        int64_t openTime = 0;
        if (!parseLeadingInt64(line, &openTime)) {
            continue;
        }
        if (openTime == asofMs) {
            return true;
        }
    }
    return false;
}

bool CandleSchedulerThread::processCandle(int64_t candleOpenTimeMs) {
    if (!datasetContainsAsofMs(candleOpenTimeMs)) {
        Logger::instance().log(
            LogLevel::Warning,
            "[CANDLE][PHASE3][SKIPPED] asof=" + std::to_string(candleOpenTimeMs) +
                " reason=dataset_missing_asof");
        return false;
    }
    const auto modelPath = resolvePublishedModelPath();
    if (!modelPath) {
        Logger::instance().log(
            LogLevel::Warning,
            "[CANDLE][PHASE3][SKIPPED] asof=" + std::to_string(candleOpenTimeMs) +
                " reason=active_model_unavailable");
        return false;
    }

    const auto phase3 = m_runner.spawnWithRetry(buildPhase3Cmd(candleOpenTimeMs, *modelPath));
    Logger::instance().log(
        phase3.succeeded ? LogLevel::Info : LogLevel::Warning,
        "[CANDLE][PHASE3][" + std::string(phase3.succeeded ? "SUCCESS" : "FAILED") + "] asof=" +
            std::to_string(candleOpenTimeMs) +
            " exit=" + std::to_string(phase3.exitCode));
    if (!phase3.succeeded) {
        return false;
    }
    (void)m_promoter.evaluate(m_stateStore);
    return true;
}

void CandleSchedulerThread::run(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        int64_t nextCandle = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::seconds(1), [&]() {
                return m_pendingCandleMs > m_lastProcessedMs;
            });
            if (stopToken.stop_requested()) {
                return;
            }
            if (m_pendingCandleMs <= m_lastProcessedMs) {
                continue;
            }
            nextCandle = m_pendingCandleMs;
        }

        for (int i = 0; i < m_config.postCandleDelaySeconds && !stopToken.stop_requested(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (stopToken.stop_requested()) {
            return;
        }
        const bool processed = processCandle(nextCandle);

        if (processed) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastProcessedMs = std::max(m_lastProcessedMs, nextCandle);
        }
    }
}

} // namespace orchestration
