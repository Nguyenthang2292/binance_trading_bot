#pragma once

#include "orchestration/runtime_ports.h"
#include "types/market.h"

#include <mutex>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace orchestration {

struct CostModelConfig {
    double estimatedRoundTripFeeBps{8.0};
    double estimatedSlippageBps{2.0};
    double estimatedFundingBpsPerDay{0.0};
};

struct ShadowMetricsConfig {
    std::string dbPath{"data/qlib_predictions.db"};
    std::string modelId{"lightgbm_1h_v1"};
    std::string interval{"1h"};
    int horizonBars{4};
    CostModelConfig costModel;
};

class ShadowMetricsRecorder final : public IShadowMetricsPort {
public:
    explicit ShadowMetricsRecorder(ShadowMetricsConfig config);
    ~ShadowMetricsRecorder();

    ShadowMetricsRecorder(const ShadowMetricsRecorder&) = delete;
    ShadowMetricsRecorder& operator=(const ShadowMetricsRecorder&) = delete;

    void initializeSchema();
    void recordShadowSignal(const ShadowSignalRecord& record) override;
    void onCandleClosed(std::string_view symbol, std::string_view interval, const Kline& kline);

private:
    static std::string directionToDb(strategy::Signal::Direction direction);
    static std::string modeToDb(ExecutionMode mode);
    static int64_t intervalToMs(const std::string& interval);
    sqlite3_stmt* predictionLookupStmtLocked();
    sqlite3_stmt* insertShadowSignalStmtLocked();
    void finalizeStatements() noexcept;
    void upsertCandleLocked(std::string_view symbol, std::string_view interval, const Kline& kline);
    void upsertActualReturnsLocked(std::string_view interval);
    void upsertShadowOutcomesLocked(std::string_view interval);
    int64_t nowMs() const;
    double fundingCost(double durationDays) const;
    std::string costModelVersion() const;

    ShadowMetricsConfig m_config;
    sqlite3* m_db{nullptr};
    sqlite3_stmt* m_predictionLookupStmt{nullptr};
    sqlite3_stmt* m_insertShadowSignalStmt{nullptr};
    std::mutex m_mutex;
};

} // namespace orchestration
