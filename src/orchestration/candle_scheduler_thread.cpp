#include "orchestration/candle_scheduler_thread.h"

#include "logger.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <ctime>
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

std::string_view trimWs(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '"')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '"')) {
        sv.remove_suffix(1);
    }
    return sv;
}

// Return the nth comma-separated field (trimmed) of `line`, or empty if absent.
std::string_view nthCsvField(std::string_view line, int index) {
    int col = 0;
    size_t start = 0;
    while (col < index) {
        const auto comma = line.find(',', start);
        if (comma == std::string_view::npos) {
            return {};
        }
        start = comma + 1;
        ++col;
    }
    const auto comma = line.find(',', start);
    const auto field = comma == std::string_view::npos ? line.substr(start) : line.substr(start, comma - start);
    return trimWs(field);
}

// Index of the `datetime` column in a CSV header, or -1 if not present.
int datetimeColumnIndex(std::string_view header) {
    int col = 0;
    size_t start = 0;
    while (true) {
        const auto comma = header.find(',', start);
        const auto field = comma == std::string_view::npos ? header.substr(start) : header.substr(start, comma - start);
        std::string lowered;
        for (const char c : trimWs(field)) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (lowered == "datetime") {
            return col;
        }
        if (comma == std::string_view::npos) {
            return -1;
        }
        start = comma + 1;
        ++col;
    }
}

// Parse "YYYY-MM-DD HH:MM:SS" (or 'T' separator, optional trailing 'Z', or
// date-only) as UTC and return epoch milliseconds. Matches predict_latest.py's
// `pd.to_datetime(..., utc=True)` convention.
bool parseDatetimeUtcMs(std::string_view token, int64_t* out) {
    if (!out || token.empty()) {
        return false;
    }
    const std::string str(token);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    char sep = 0;
    const int matched = std::sscanf(str.c_str(), "%d-%d-%d%c%d:%d:%d", &y, &mo, &d, &sep, &h, &mi, &s);
    if (matched < 3) {
        return false;
    }
    if (matched >= 4 && sep != ' ' && sep != 'T' && sep != 't') {
        return false;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60) {
        return false;
    }
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = 0;
#if defined(_WIN32)
    const std::time_t t = _mkgmtime(&tm);
#else
    const std::time_t t = timegm(&tm);
#endif
    if (t == static_cast<std::time_t>(-1)) {
        return false;
    }
    *out = static_cast<int64_t>(t) * 1000;
    return true;
}

std::string formatUtcMs(int64_t ms) {
    if (ms <= 0) {
        return "n/a";
    }
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
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

std::vector<std::string> CandleSchedulerThread::buildRefreshCmd(int64_t asofMs) const {
    std::vector<std::string> cmd;
    cmd.reserve(16 + m_config.symbols.size());
    cmd.push_back(m_config.pythonExe);
    cmd.push_back((std::filesystem::path(m_config.scriptsDir) / "refresh_latest_candles.py").string());
    cmd.push_back("--symbols");
    for (const auto& symbol : m_config.symbols) {
        cmd.push_back(symbol);
    }
    cmd.push_back("--interval");
    cmd.push_back(m_config.interval);
    cmd.push_back("--asof-ms");
    cmd.push_back(std::to_string(asofMs));
    cmd.push_back("--dataset");
    cmd.push_back((std::filesystem::path(m_config.dataDir) / ("klines_" + m_config.interval + ".csv")).string());
    cmd.push_back("--db-path");
    cmd.push_back(m_config.dbPath);
    cmd.push_back("--merge-mode");
    cmd.push_back("upsert");
    return cmd;
}

bool CandleSchedulerThread::refreshLatestCandle(int64_t asofMs) {
    if (!m_config.refreshLatestCandles || m_config.symbols.empty()) {
        return false;
    }
    const auto refresh = m_runner.spawnWithRetry(buildRefreshCmd(asofMs));
    Logger::instance().log(
        refresh.succeeded ? LogLevel::Info : LogLevel::Warning,
        "[CANDLE][PHASE3][REFRESH][" + std::string(refresh.succeeded ? "OK" : "FAILED") + "] asof=" +
            std::to_string(asofMs) + " exit=" + std::to_string(refresh.exitCode));
    return refresh.succeeded;
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

CandleSchedulerThread::DatasetAsofScan CandleSchedulerThread::scanDatasetForAsof(int64_t asofMs) const {
    DatasetAsofScan scan;
    if (asofMs <= 0) {
        return scan;
    }
    const std::filesystem::path datasetPath =
        std::filesystem::path(m_config.dataDir) / ("klines_" + m_config.interval + ".csv");
    std::ifstream input(datasetPath, std::ios::binary);
    if (!input) {
        return scan;  // readable == false
    }
    scan.readable = true;

    std::string header;
    if (!std::getline(input, header)) {
        return scan;  // empty file
    }

    // Fix B: support both the runtime Qlib bridge schema (a `datetime` column,
    // parsed as UTC to match predict_latest.py) and the legacy epoch-ms schema
    // (leading integer column). The previous implementation only handled the
    // latter and silently false-negatived on the real runtime dataset.
    const int datetimeIdx = datetimeColumnIndex(header);

    auto consider = [&](std::string_view line) {
        int64_t rowMs = 0;
        bool ok = false;
        if (datetimeIdx >= 0) {
            const auto field = nthCsvField(line, datetimeIdx);
            ok = parseDatetimeUtcMs(field, &rowMs);
        } else {
            ok = parseLeadingInt64(line, &rowMs);
        }
        if (!ok) {
            return;
        }
        if (rowMs > scan.maxAsofMs) {
            scan.maxAsofMs = rowMs;
        }
        if (rowMs == asofMs) {
            scan.found = true;
        }
    };

    // A legacy file may have no header row (first line is already epoch-ms data).
    if (datetimeIdx < 0) {
        int64_t probe = 0;
        if (parseLeadingInt64(header, &probe)) {
            consider(header);
        }
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        consider(line);
    }
    return scan;
}

bool CandleSchedulerThread::processCandle(int64_t candleOpenTimeMs) {
    try {
        // Fix A: refresh the just-closed candle into the runtime dataset before
        // checking/predicting. Fail-soft: even if refresh fails (e.g. transient
        // network/proxy), still proceed — the dataset may already contain the
        // asof from another job, and the scan below is the source of truth.
        const bool refreshed = refreshLatestCandle(candleOpenTimeMs);

        const auto scan = scanDatasetForAsof(candleOpenTimeMs);
        if (!scan.found) {
            const std::filesystem::path datasetPath =
                std::filesystem::path(m_config.dataDir) / ("klines_" + m_config.interval + ".csv");
            // Fix D: rich diagnostics — distinguish "stale dataset" from "refresh
            // could not produce the row" and surface the dataset's latest asof.
            const char* reason = !scan.readable ? "dataset_unreadable"
                : (m_config.refreshLatestCandles && !m_config.symbols.empty() && !refreshed)
                    ? "dataset_refresh_failed"
                    : "dataset_missing_asof";
            Logger::instance().log(
                LogLevel::Warning,
                "[CANDLE][PHASE3][SKIPPED] asof=" + std::to_string(candleOpenTimeMs) +
                    " asof_utc=" + formatUtcMs(candleOpenTimeMs) +
                    " dataset=" + datasetPath.string() +
                    " dataset_max=" + formatUtcMs(scan.maxAsofMs) +
                    " reason=" + reason);
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
    } catch (const std::exception& e) {
        Logger::instance().log(
            LogLevel::Warning,
            "[CANDLE][PHASE3][FAILED] asof=" + std::to_string(candleOpenTimeMs) +
                " reason=exception:" + e.what());
        return false;
    } catch (...) {
        Logger::instance().log(
            LogLevel::Warning,
            "[CANDLE][PHASE3][FAILED] asof=" + std::to_string(candleOpenTimeMs) +
                " reason=unknown_exception");
        return false;
    }
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
        bool processed = false;
        try {
            processed = processCandle(nextCandle);
        } catch (const std::exception& e) {
            Logger::instance().log(
                LogLevel::Warning,
                "[CANDLE][THREAD][FAILED] asof=" + std::to_string(nextCandle) +
                    " reason=exception:" + e.what());
        } catch (...) {
            Logger::instance().log(
                LogLevel::Warning,
                "[CANDLE][THREAD][FAILED] asof=" + std::to_string(nextCandle) +
                    " reason=unknown_exception");
        }

        if (processed) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastProcessedMs = std::max(m_lastProcessedMs, nextCandle);
            m_retryAsofMs = 0;
            m_retryCount = 0;
            continue;
        }

        // Bounded retry (anti-spin): without this, a candle that can never be
        // satisfied (e.g. asof in the future, delisted symbol, permanently stale
        // upstream) is retried every loop forever, flooding logs and masking
        // other warnings. After maxRetriesPerCandle consecutive failures on the
        // same asof, advance the cursor and move on.
        if (m_config.maxRetriesPerCandle > 0) {
            if (nextCandle != m_retryAsofMs) {
                m_retryAsofMs = nextCandle;
                m_retryCount = 0;
            }
            ++m_retryCount;
            if (m_retryCount >= m_config.maxRetriesPerCandle) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_lastProcessedMs = std::max(m_lastProcessedMs, nextCandle);
                }
                Logger::instance().log(
                    LogLevel::Warning,
                    "[CANDLE][PHASE3][GAVE_UP] asof=" + std::to_string(nextCandle) +
                        " attempts=" + std::to_string(m_retryCount) +
                        " reason=give_up_after_retries");
                m_retryAsofMs = 0;
                m_retryCount = 0;
            }
        }
    }
}

} // namespace orchestration
