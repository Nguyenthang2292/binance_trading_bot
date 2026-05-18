#pragma once

#include "risk/risk_types.h"

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace engine {

class RiskDb {
public:
    explicit RiskDb(const std::string& dbPath);
    ~RiskDb();

    RiskDb(const RiskDb&) = delete;
    RiskDb& operator=(const RiskDb&) = delete;

    void insertEquityPoint(const EquityPoint& p);
    std::vector<EquityPoint> getByYear(int year) const;
    std::vector<EquityPoint> getByTimeRange(
        std::string_view basis,
        int64_t startMs,
        int64_t endMs) const;

    void insertMetrics(const RiskMetricsResult& m);
    std::optional<RiskMetricsResult> getLatestMetrics(
        std::string_view windowKind,
        std::string_view basis) const;
    void deleteEquityPointsOlderThan(int64_t cutoffMs);

private:
    void initSchema();

    sqlite3* m_db{nullptr};
    mutable std::mutex m_mutex;
};

} // namespace engine
