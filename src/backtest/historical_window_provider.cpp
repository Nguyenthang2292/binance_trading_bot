#include "backtest/historical_window_provider.h"
#include "logger.h"
#include "scanner/kline_cache.h"

#include <algorithm>

namespace backtest {

HistoricalWindowProvider::HistoricalWindowProvider(
    const scanner::KlineCache& cache,
    BacktestGateDataConfig config)
    : m_cache(cache), m_config(std::move(config)) {}

IHistoricalWindowProvider::WindowResult HistoricalWindowProvider::closedWindow(
    std::string_view symbol,
    std::string_view interval,
    int requiredClosedBars,
    std::chrono::system_clock::time_point signalBarOpenTime) const {

    WindowResult result;
    result.requiredBars = requiredClosedBars;
    result.sufficient = false;
    result.availableBars = 0;

    if (requiredClosedBars <= 0) {
        return result;
    }

    auto snapshotOpt = m_cache.snapshot(symbol, interval);
    if (!snapshotOpt) {
        return result;
    }

    const auto& allKlines = *snapshotOpt;
    const long long signalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        signalBarOpenTime.time_since_epoch()).count();

    // Keep closed bars up to and including signal bar T.
    std::vector<Kline> filtered;
    filtered.reserve(allKlines.size());
    for (const auto& kline : allKlines) {
        if (!kline.isClosed) continue;
        if (kline.openTime > signalTimeMs) continue;
        filtered.push_back(kline);
    }

    result.availableBars = static_cast<int>(filtered.size());

    if (result.availableBars < requiredClosedBars) {
        return result;
    }

    // Last bar must be the signal bar T.
    if (filtered.back().openTime != signalTimeMs) {
        // Cache does not contain the signal bar yet; treat as insufficient.
        return result;
    }

    result.sufficient = true;
    auto startIt = filtered.end() - requiredClosedBars;
    result.closedKlines.assign(startIt, filtered.end());

    if (m_config.historySource == "cache_then_rest") {
        // v1.2 will add REST back-fill here; for now fall through with cache data.
        Logger::instance().log(LogLevel::Warning,
            "backtest_gate: history_source=cache_then_rest is not yet implemented; "
            "using cache_only behaviour for symbol=" + std::string(symbol) +
            " interval=" + std::string(interval));
    }

    return result;
}

}  // namespace backtest
