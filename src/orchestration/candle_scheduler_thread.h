#pragma once

#include "orchestration/process_manager.h"
#include "orchestration/promotion_checker.h"
#include "orchestration/qlib_state_store.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
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
    // Qlib symbol universe used to refresh the latest closed candle before a
    // Phase 3 prediction (Fix A). Empty disables the refresh step.
    std::vector<std::string> symbols;
    // Run refresh_latest_candles.py before predicting so the runtime dataset
    // contains the just-closed candle (Fix A).
    bool refreshLatestCandles{true};
    // Bounded retry: after this many consecutive failed attempts on the SAME
    // asof, the cursor advances so the thread stops re-processing it forever
    // (anti-spin). 0 disables the cap (retry indefinitely).
    int maxRetriesPerCandle{5};
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
    std::vector<std::string> buildRefreshCmd(int64_t asofMs) const;
    bool processCandle(int64_t candleOpenTimeMs);

private:
    // Result of scanning the runtime dataset for a given asof open time.
    struct DatasetAsofScan {
        bool readable{false};   // dataset file opened successfully
        bool found{false};      // asof present for ALL required symbols (or any row when no symbol coverage check applies)
        int64_t maxAsofMs{0};   // latest asof (epoch ms) seen in the dataset
        std::string missingSymbols;  // configured symbols with no row at asofMs (diagnostics)
    };

    std::vector<std::string> buildPhase3Cmd(int64_t asofMs, std::string_view modelPath) const;
    std::optional<std::string> resolvePublishedModelPath() const;
    DatasetAsofScan scanDatasetForAsof(int64_t asofMs) const;
    bool refreshLatestCandle(int64_t asofMs);

    CandleSchedulerConfig m_config;
    IProcessRunner& m_runner;
    QlibStateStore& m_stateStore;
    PromotionChecker& m_promoter;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    int64_t m_pendingCandleMs{0};
    int64_t m_lastProcessedMs{0};

    // Bounded-retry bookkeeping (accessed only on the scheduler thread).
    int64_t m_retryAsofMs{0};
    int m_retryCount{0};
};

} // namespace orchestration
