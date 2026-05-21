#include "orchestration/batch_scheduler_thread.h"

#include "logger.h"
#include "orchestration/model_publisher.h"
#include "orchestration/sqlite_helpers.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <thread>

namespace orchestration {

namespace {

int64_t nowMs() {
    return sqlite_helpers::nowMs();
}

std::tm localTime(std::time_t t) {
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

std::string isoNowCompact() {
    const auto now = std::time(nullptr);
    const std::tm tmNow = localTime(now);
    std::ostringstream out;
    out << std::put_time(&tmNow, "%Y%m%dT%H%M%S");
    return out.str();
}

bool containsWeekday(const std::vector<int>& days, int monZeroWeekday) {
    return std::find(days.begin(), days.end(), monZeroWeekday) != days.end();
}

bool pathPartEqual(const std::filesystem::path& left, const std::filesystem::path& right) {
#if defined(_WIN32)
    std::string leftText = left.string();
    std::string rightText = right.string();
    std::transform(leftText.begin(), leftText.end(), leftText.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    std::transform(rightText.begin(), rightText.end(), rightText.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return leftText == rightText;
#else
    return left == right;
#endif
}

bool isPathWithin(const std::filesystem::path& child, const std::filesystem::path& parent) {
    std::error_code ec;
    const auto childPath = std::filesystem::weakly_canonical(child, ec);
    if (ec) {
        return false;
    }
    ec.clear();
    const auto parentPath = std::filesystem::weakly_canonical(parent, ec);
    if (ec) {
        return false;
    }

    auto childIt = childPath.begin();
    auto parentIt = parentPath.begin();
    for (; parentIt != parentPath.end(); ++parentIt, ++childIt) {
        if (childIt == childPath.end() || !pathPartEqual(*childIt, *parentIt)) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> validatedDumpBinScript(
    const BatchSchedulerConfig& config,
    const std::string& runId) {
    const char* dumpBin = std::getenv("QLIB_DUMP_BIN_PATH");
    if (!dumpBin || !*dumpBin) {
        return std::nullopt;
    }

    std::error_code ec;
    const auto resolved = std::filesystem::weakly_canonical(std::filesystem::path(dumpBin), ec);
    if (ec || !std::filesystem::is_regular_file(resolved, ec)) {
        Logger::instance().log(
            LogLevel::Warning,
            "[BATCH][PHASE1] Ignoring invalid QLIB_DUMP_BIN_PATH=" + std::string(dumpBin));
        return std::nullopt;
    }

    const auto cwd = std::filesystem::current_path(ec);
    const std::vector<std::filesystem::path> allowedRoots = {
        std::filesystem::path(config.scriptsDir),
        cwd / "tools" / "qlib_bridge",
        cwd / "scripts",
    };
    const bool allowed = std::any_of(allowedRoots.begin(), allowedRoots.end(), [&](const auto& root) {
        return isPathWithin(resolved, root);
    });
    if (!allowed) {
        Logger::instance().log(
            LogLevel::Warning,
            "[BATCH][PHASE1] Rejecting QLIB_DUMP_BIN_PATH outside allowed script roots: " + resolved.string());
        return std::nullopt;
    }

    Logger::instance().log(
        LogLevel::Info,
        "[BATCH][PHASE1] Using QLIB_DUMP_BIN_PATH=" + resolved.string() +
            (runId.empty() ? std::string{} : " run_id=" + runId));
    return resolved.string();
}

} // namespace

BatchSchedulerThread::BatchSchedulerThread(BatchSchedulerConfig config, IProcessRunner& runner)
    : m_config(std::move(config)),
      m_runner(runner) {
    if (m_config.batchWeekdays.empty()) {
        m_config.batchWeekdays = {0, 1, 2, 3, 4};
    }
    m_config.horizonBars = std::max(1, m_config.horizonBars);
    if (m_config.logDir.empty()) {
        m_config.logDir = "logs/qlib_orch";
    }
}

bool BatchSchedulerThread::shouldRunToday(std::chrono::system_clock::time_point now) const {
    const auto nowT = std::chrono::system_clock::to_time_t(now);
    const std::tm tmNow = localTime(nowT);
    const int monZeroWeekday = (tmNow.tm_wday + 6) % 7;
    if (!containsWeekday(m_config.batchWeekdays, monZeroWeekday)) {
        return false;
    }
    if (tmNow.tm_hour < m_config.batchHour) {
        return false;
    }
    if (tmNow.tm_hour == m_config.batchHour && tmNow.tm_min < m_config.batchMinute) {
        return false;
    }
    return true;
}

std::string BatchSchedulerThread::todayDateStr(std::chrono::system_clock::time_point now) const {
    const auto nowT = std::chrono::system_clock::to_time_t(now);
    const std::tm tmNow = localTime(nowT);
    std::ostringstream out;
    out << std::put_time(&tmNow, "%Y-%m-%d");
    return out.str();
}

std::vector<std::string> BatchSchedulerThread::buildPhase1Cmd() const {
    std::vector<std::string> cmd;
    cmd.reserve(32);
    cmd.push_back(m_config.pythonExe);
    cmd.push_back((std::filesystem::path(m_config.scriptsDir) / "export_binance_klines.py").string());
    cmd.push_back("--symbols");
    for (const auto& symbol : m_config.symbols) {
        cmd.push_back(symbol);
    }
    cmd.push_back("--interval");
    cmd.push_back(m_config.interval);
    cmd.push_back("--start-ms");
    const int64_t lookbackMs = 365LL * 24LL * 60LL * 60LL * 1000LL;
    cmd.push_back(std::to_string(nowMs() - lookbackMs));
    cmd.push_back("--output");
    cmd.push_back((std::filesystem::path(m_config.dataDir) / ("klines_" + m_config.interval + ".csv")).string());
    if (const auto dumpBin = validatedDumpBinScript(m_config, m_currentRunId)) {
        cmd.push_back("--convert-mode");
        cmd.push_back("incremental");
        cmd.push_back("--dump-bin-script");
        cmd.push_back(*dumpBin);
        cmd.push_back("--qlib-dir");
        cmd.push_back((std::filesystem::path(m_config.dataDir) / "qlib_bin").string());
    } else {
        cmd.push_back("--convert-mode");
        cmd.push_back("none");
    }
    return cmd;
}

std::vector<std::string> BatchSchedulerThread::buildPhase2Cmd() const {
    std::vector<std::string> cmd;
    cmd.reserve(24);
    cmd.push_back(m_config.pythonExe);
    cmd.push_back((std::filesystem::path(m_config.scriptsDir) / "train_workflow.py").string());
    cmd.push_back("--dataset");
    cmd.push_back((std::filesystem::path(m_config.dataDir) / ("klines_" + m_config.interval + ".csv")).string());
    cmd.push_back("--model-out");
    cmd.push_back((std::filesystem::path(m_config.modelDir) / "model_staging" / m_currentRunId / "model.txt").string());
    cmd.push_back("--report-out");
    cmd.push_back((std::filesystem::path(m_config.modelDir) / "model_staging" / m_currentRunId / "report.json").string());
    cmd.push_back("--interval");
    cmd.push_back(m_config.interval);
    cmd.push_back("--horizon-bars");
    cmd.push_back(std::to_string(m_config.horizonBars));
    return cmd;
}

void BatchSchedulerThread::pruneOldLogs() const {
    if (m_config.logRetentionDays <= 0) {
        return;
    }
    const std::filesystem::path logDir(m_config.logDir);
    if (!std::filesystem::exists(logDir)) {
        return;
    }
    const auto now = std::filesystem::file_time_type::clock::now();
    const auto cutoff = now - std::chrono::hours(24 * m_config.logRetentionDays);
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(logDir, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }
        const auto writeTime = entry.last_write_time(ec);
        if (ec) {
            continue;
        }
        if (writeTime < cutoff) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

bool BatchSchedulerThread::runOnce() {
    pruneOldLogs();
    m_currentRunId = m_config.modelId + "_" + isoNowCompact();

    const auto phase1 = m_runner.spawnWithRetry(buildPhase1Cmd());
    Logger::instance().log(
        phase1.succeeded ? LogLevel::Info : LogLevel::Warning,
        "[BATCH][PHASE1][" + std::string(phase1.succeeded ? "SUCCESS" : "FAILED") + "] "
            "exit=" + std::to_string(phase1.exitCode) +
            " log=" + phase1.logPath);
    if (!phase1.succeeded) {
        Logger::instance().log(LogLevel::Warning, "[BATCH][PHASE2][SKIP] Phase 1 failed");
        return false;
    }

    const auto phase2 = m_runner.spawnWithRetry(buildPhase2Cmd());
    Logger::instance().log(
        phase2.succeeded ? LogLevel::Info : LogLevel::Warning,
        "[BATCH][PHASE2][" + std::string(phase2.succeeded ? "SUCCESS" : "FAILED") + "] "
            "exit=" + std::to_string(phase2.exitCode) +
            " log=" + phase2.logPath);
    if (!phase2.succeeded) {
        return false;
    }

    ModelPublishRequest publishRequest;
    publishRequest.dbPath = m_config.dbPath;
    publishRequest.modelId = m_config.modelId;
    publishRequest.interval = m_config.interval;
    publishRequest.runId = m_currentRunId;
    publishRequest.horizonBars = m_config.horizonBars;
    publishRequest.stagingDir = (std::filesystem::path(m_config.modelDir) / "model_staging").string();
    publishRequest.artifactsDir = (std::filesystem::path(m_config.modelDir) / "model_artifacts").string();
    publishRequest.manifestPath =
        (std::filesystem::path(m_config.modelDir) / "current" / (m_config.modelId + ".json")).string();

    std::string publishError;
    if (!ModelPublisher::publish(publishRequest, publishError)) {
        Logger::instance().log(
            LogLevel::Error,
            "[BATCH][PUBLISH][FAILED] run_id=" + m_currentRunId + " error=" + publishError);
        return false;
    }
    Logger::instance().log(LogLevel::Info, "[BATCH][PUBLISH][SUCCESS] run_id=" + m_currentRunId);
    return true;
}

bool BatchSchedulerThread::runScheduledCycleAt(std::chrono::system_clock::time_point now) {
    const std::string today = todayDateStr(now);
    if (!shouldRunToday(now) || m_lastRunDate == today) {
        return false;
    }
    const bool ok = runOnce();
    m_lastRunDate = today;
    if (!ok) {
        Logger::instance().log(LogLevel::Warning, "[BATCH] cycle failed");
    }
    return true;
}

std::chrono::system_clock::time_point BatchSchedulerThread::nextWakeTime(
    std::chrono::system_clock::time_point now) const {
    const auto nowT = std::chrono::system_clock::to_time_t(now);
    const std::tm base = localTime(nowT);

    for (int dayOffset = 0; dayOffset < 14; ++dayOffset) {
        std::tm candidateTm = base;
        candidateTm.tm_mday += dayOffset;
        candidateTm.tm_hour = m_config.batchHour;
        candidateTm.tm_min = m_config.batchMinute;
        candidateTm.tm_sec = 0;
        candidateTm.tm_isdst = -1;
        const std::time_t candidateT = std::mktime(&candidateTm);
        if (candidateT == static_cast<std::time_t>(-1)) {
            continue;
        }
        const std::tm normalized = localTime(candidateT);
        const int monZeroWeekday = (normalized.tm_wday + 6) % 7;
        if (!containsWeekday(m_config.batchWeekdays, monZeroWeekday)) {
            continue;
        }
        const auto candidate = std::chrono::system_clock::from_time_t(candidateT);
        if (candidate <= now) {
            continue;
        }
        if (todayDateStr(candidate) == m_lastRunDate) {
            continue;
        }
        return candidate;
    }

    return now + std::chrono::hours(1);
}

void BatchSchedulerThread::run(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        const auto now = std::chrono::system_clock::now();
        (void)runScheduledCycleAt(now);

        const auto wakeAt = nextWakeTime(std::chrono::system_clock::now());
        std::unique_lock<std::mutex> lock(m_sleepMutex);
        std::stop_callback stopWake(stopToken, [this]() {
            m_sleepCv.notify_all();
        });
        m_sleepCv.wait_until(lock, wakeAt, [&]() {
            return stopToken.stop_requested();
        });
    }
}

} // namespace orchestration
