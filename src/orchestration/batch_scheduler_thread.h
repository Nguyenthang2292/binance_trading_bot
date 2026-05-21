#pragma once

#include "orchestration/process_manager.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace orchestration {

struct BatchSchedulerConfig {
    std::string pythonExe;
    std::string scriptsDir;
    std::string dataDir;
    std::string modelDir;
    std::string dbPath;
    std::string modelId;
    std::string interval;
    std::vector<std::string> symbols;
    int horizonBars{4};
    int batchHour{7};
    int batchMinute{0};
    std::vector<int> batchWeekdays{0, 1, 2, 3, 4};
    int logRetentionDays{14};
    std::string logDir{"logs/qlib_orch"};
};

class BatchSchedulerThread {
public:
    BatchSchedulerThread(BatchSchedulerConfig config, IProcessRunner& runner);

    void run(std::stop_token stopToken);
    bool runOnce();
    bool runScheduledCycleAt(std::chrono::system_clock::time_point now);

    bool shouldRunToday(std::chrono::system_clock::time_point now) const;
    std::string todayDateStr(std::chrono::system_clock::time_point now) const;
    std::vector<std::string> buildPhase1Cmd() const;
    std::vector<std::string> buildPhase2Cmd() const;
    void pruneOldLogs() const;

private:
    std::chrono::system_clock::time_point nextWakeTime(std::chrono::system_clock::time_point now) const;

    BatchSchedulerConfig m_config;
    IProcessRunner& m_runner;
    std::string m_lastRunDate;
    std::string m_currentRunId;
    mutable std::mutex m_sleepMutex;
    std::condition_variable m_sleepCv;
};

} // namespace orchestration
