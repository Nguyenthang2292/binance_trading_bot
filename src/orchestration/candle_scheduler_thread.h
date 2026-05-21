#pragma once

#include "orchestration/process_manager.h"
#include "orchestration/promotion_checker.h"
#include "orchestration/qlib_state_store.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace orchestration {

struct CandleSchedulerConfig {
    std::string pythonExe;
    std::string scriptsDir;
    std::string dataDir;
    std::string modelDir;
    std::string dbPath;
    std::string modelId;
    std::string interval;
    int postCandleDelaySeconds{60};
    std::string readyDir{"tmp/qlib_signals"};
};

class CandleSchedulerThread {
public:
    CandleSchedulerThread(
        CandleSchedulerConfig config,
        IProcessRunner& runner,
        QlibStateStore& stateStore,
        PromotionChecker& promoter);

    void notifyCandleClose(int64_t candleOpenTimeMs, std::string_view symbol);
    void run(std::stop_token stopToken);

    std::vector<std::string> buildPhase3Cmd(int64_t asofMs) const;
    void processCandle(int64_t candleOpenTimeMs);

private:
    CandleSchedulerConfig m_config;
    IProcessRunner& m_runner;
    QlibStateStore& m_stateStore;
    PromotionChecker& m_promoter;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    int64_t m_pendingCandleMs{0};
    int64_t m_lastProcessedMs{0};
};

} // namespace orchestration
