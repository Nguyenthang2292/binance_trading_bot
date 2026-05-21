#include "orchestration/candle_scheduler_thread.h"

#include "logger.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

namespace orchestration {

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
    std::vector<std::string> cmd;
    cmd.reserve(24);
    cmd.push_back(m_config.pythonExe);
    cmd.push_back((std::filesystem::path(m_config.scriptsDir) / "predict_latest.py").string());
    cmd.push_back("--dataset");
    cmd.push_back((std::filesystem::path(m_config.dataDir) / ("klines_" + m_config.interval + ".csv")).string());
    cmd.push_back("--model-path");
    cmd.push_back((std::filesystem::path(m_config.modelDir) / ("lightgbm_" + m_config.interval + ".txt")).string());
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

void CandleSchedulerThread::processCandle(int64_t candleOpenTimeMs) {
    const auto phase3 = m_runner.spawnWithRetry(buildPhase3Cmd(candleOpenTimeMs));
    Logger::instance().log(
        phase3.succeeded ? LogLevel::Info : LogLevel::Warning,
        "[CANDLE][PHASE3][" + std::string(phase3.succeeded ? "SUCCESS" : "FAILED") + "] asof=" +
            std::to_string(candleOpenTimeMs) +
            " exit=" + std::to_string(phase3.exitCode));
    if (!phase3.succeeded) {
        return;
    }
    (void)m_promoter.evaluate(m_stateStore);
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
        processCandle(nextCandle);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastProcessedMs = std::max(m_lastProcessedMs, nextCandle);
        }
    }
}

} // namespace orchestration
