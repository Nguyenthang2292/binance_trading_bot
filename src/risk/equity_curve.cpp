#include "risk/equity_curve.h"

#include "logger.h"

#include <chrono>
#include <cmath>
#include <sstream>

namespace engine {

EquityCurve::EquityCurve(RiskDb& db) : m_db(db) {}

void EquityCurve::recordTradeClose(double equity, int64_t timestampMs, std::string_view basis) {
    record(equity, timestampMs, "trade_close", basis);
}

void EquityCurve::recordPeriodic(double equity, int64_t timestampMs, std::string_view basis) {
    record(equity, timestampMs, "periodic", basis);
}

std::vector<EquityPoint> EquityCurve::getByYear(int year) const {
    return m_db.getByYear(year);
}

std::vector<EquityPoint> EquityCurve::getByTimeRange(std::string_view basis, int64_t startMs, int64_t endMs) const {
    return m_db.getByTimeRange(basis, startMs, endMs);
}

void EquityCurve::record(double equity, int64_t timestampMs, std::string_view source, std::string_view basis) {
    if (!std::isfinite(equity) || equity <= 0.0 || timestampMs <= 0 || basis.empty()) {
        std::ostringstream out;
        out << "risk equity point dropped"
            << " equity=" << equity
            << " timestamp_ms=" << timestampMs
            << " source=" << source
            << " basis=" << basis;
        Logger::instance().log(LogLevel::Warning, out.str());
        return;
    }
    EquityPoint p;
    p.timestampMs = timestampMs;
    p.equity = equity;
    p.year = extractYear(timestampMs);
    p.source = std::string(source);
    p.basis = std::string(basis);
    m_db.insertEquityPoint(p);
}

int EquityCurve::extractYear(int64_t timestampMs) {
    const auto tp = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestampMs));
    const auto dp = std::chrono::floor<std::chrono::days>(tp);
    const std::chrono::year_month_day ymd{dp};
    return static_cast<int>(ymd.year());
}

} // namespace engine
