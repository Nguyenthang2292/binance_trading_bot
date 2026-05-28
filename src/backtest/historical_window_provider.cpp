/**
 * @file historical_window_provider.cpp
 * @brief Cache-only closed-window provider implementation.
 */

#include "backtest/historical_window_provider.h"
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
    /**
     * @brief Build and return a closed-window result from cached klines.
     *
     * This method queries the `KlineCache` for a snapshot of klines for the
     * provided `symbol` and `interval`. It filters for closed klines whose
     * open time is <= the provided `signalBarOpenTime` (inclusive). If the
     * cache does not contain enough closed bars, or if the latest closed bar
     * at the signal time is missing, the returned `WindowResult` will have
     * `sufficient == false` and `availableBars` indicating how many closed
     * bars were found.
     *
     * @param symbol Symbol to query (e.g. "BTCUSDT").
     * @param interval Kline interval string (e.g. "1m").
     * @param requiredClosedBars Number of closed bars required for the
     *        window. If <= 0 the function returns early with insufficient
     *        result.
     * @param signalBarOpenTime The open time of the signal bar (as
     *        `std::chrono::system_clock::time_point`). Only closed bars with
     *        openTime <= this time are considered; the latest closed bar must
     *        have openTime equal to this value for the window to be
     *        considered sufficient.
     *
     * @return IHistoricalWindowProvider::WindowResult Populated result. The
     *         `source` field is set to "cache" for this provider.
     */

    WindowResult result;
    result.requiredBars = requiredClosedBars;
    result.sufficient = false;
    result.availableBars = 0;
    result.source = "cache";

    // Trivial rejection for non-positive requirements.
    if (requiredClosedBars <= 0) {
        return result;
    }

    // Fetch snapshot from cache; if missing, we cannot satisfy the request.
    auto snapshotOpt = m_cache.snapshot(symbol, interval);
    if (!snapshotOpt) {
        return result;
    }

    const auto& allKlines = *snapshotOpt;
    const long long signalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        signalBarOpenTime.time_since_epoch()).count();

    // Filter closed klines with openTime <= signalTimeMs and preserve order.
    std::vector<Kline> filtered;
    filtered.reserve(allKlines.size());
    for (const auto& kline : allKlines) {
        if (!kline.isClosed) continue;
        if (kline.openTime > signalTimeMs) continue;
        filtered.push_back(kline);
    }

    result.availableBars = static_cast<int>(filtered.size());

    // Not enough closed bars available.
    if (result.availableBars < requiredClosedBars) {
        return result;
    }

    // The most recent closed bar must correspond to the signal bar time.
    if (filtered.back().openTime != signalTimeMs) {
        // Cache does not contain the signal bar yet; treat as insufficient.
        return result;
    }

    // We have enough bars and the last bar matches the signal bar; populate
    // the closedKlines window with the last `requiredClosedBars` entries.
    result.sufficient = true;
    auto startIt = filtered.end() - requiredClosedBars;
    result.closedKlines.assign(startIt, filtered.end());

    return result;
}

}  // namespace backtest
