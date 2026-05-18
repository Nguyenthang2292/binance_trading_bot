#pragma once

#include "risk/risk_db.h"

#include <string_view>
#include <vector>

namespace engine {

class EquityCurve {
public:
    explicit EquityCurve(RiskDb& db);

    void recordTradeClose(double equity, int64_t timestampMs, std::string_view basis);
    void recordPeriodic(double equity, int64_t timestampMs, std::string_view basis);

    std::vector<EquityPoint> getByYear(int year) const;
    std::vector<EquityPoint> getByTimeRange(std::string_view basis, int64_t startMs, int64_t endMs) const;

private:
    void record(double equity, int64_t timestampMs, std::string_view source, std::string_view basis);
    static int extractYear(int64_t timestampMs);

    RiskDb& m_db;
};

} // namespace engine

