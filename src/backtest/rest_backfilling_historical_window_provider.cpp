/**
 * @file rest_backfilling_historical_window_provider.cpp
 * @brief Cache-first provider that performs REST backfill when needed.
 */

#include "backtest/rest_backfilling_historical_window_provider.h"

#include "scanner/kline_cache.h"

#include <chrono>
#include <cmath>
#include <span>
#include <utility>

namespace backtest {

namespace {

bool hasValidOhlc(const Kline& kline) {
    if (!std::isfinite(kline.open) || !std::isfinite(kline.high) ||
        !std::isfinite(kline.low) || !std::isfinite(kline.close)) {
        return false;
    }
    return kline.high >= kline.open && kline.high >= kline.close && kline.high >= kline.low &&
           kline.low <= kline.open && kline.low <= kline.close;
}

bool hasConsecutiveSpacing(std::span<const Kline> bars) {
    if (bars.size() < 2) {
        return true;
    }
    const int64_t spacing = bars[1].openTime - bars[0].openTime;
    if (spacing <= 0) {
        return false;
    }
    for (size_t i = 1; i < bars.size(); ++i) {
        if ((bars[i].openTime - bars[i - 1].openTime) != spacing) {
            return false;
        }
    }
    return true;
}

bool isSaneWindow(std::span<const Kline> bars) {
    if (!hasConsecutiveSpacing(bars)) {
        return false;
    }
    for (const auto& bar : bars) {
        if (!bar.isClosed || !hasValidOhlc(bar)) {
            return false;
        }
    }
    return true;
}

}  // namespace

/**
 * @brief Construct a cache-first historical window provider with REST fallback.
 *
 * @param innerProvider Primary provider used before falling back to REST.
 * @param restClient REST client used to backfill missing candles.
 * @param cache Shared kline cache updated after successful backfill.
 * @param config Runtime backfill settings and request limits.
 */
RestBackfillingHistoricalWindowProvider::RestBackfillingHistoricalWindowProvider(
    std::unique_ptr<IHistoricalWindowProvider> innerProvider,
    std::unique_ptr<IKlineRestClient> restClient,
    scanner::KlineCache& cache,
    BacktestGateDataConfig config)
    : m_innerProvider(std::move(innerProvider)),
      m_restClient(std::move(restClient)),
      m_cache(cache),
      m_config(std::move(config)) {}

/**
 * @brief Resolve a closed candle window, backfilling from REST when needed.
 *
 * The provider first consults the inner cache-backed provider. If that does
 * not supply enough closed bars and runtime REST fetching is enabled, it asks
 * the REST client for historical candles, validates the returned signal bar,
 * and writes the data back into the shared cache.
 *
 * @param symbol Trading symbol to inspect.
 * @param interval Candle interval string.
 * @param requiredClosedBars Number of closed bars required for the request.
 * @param signalBarOpenTime Open time of the signal bar.
 * @return A populated window result describing the available historical data.
 */
IHistoricalWindowProvider::WindowResult RestBackfillingHistoricalWindowProvider::closedWindow(
    std::string_view symbol,
    std::string_view interval,
    int requiredClosedBars,
    std::chrono::system_clock::time_point signalBarOpenTime) const {

    if (requiredClosedBars <= 0) {
        WindowResult trivial;
        trivial.sufficient = true;
        trivial.requiredBars = requiredClosedBars;
        trivial.source = "cache";
        return trivial;
    }

    auto inner = m_innerProvider->closedWindow(symbol, interval, requiredClosedBars, signalBarOpenTime);
    if (inner.sufficient) {
        if (inner.source.empty()) {
            inner.source = "cache";
        }
        return inner;
    }

    WindowResult out;
    out.requiredBars = requiredClosedBars;
    out.availableBars = inner.availableBars;
    out.source = "rest";

    if (!m_config.runtimeRestFetchEnabled) {
        out.errorReason = "rest_runtime_disabled";
        return out;
    }

    const long long signalOpenMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        signalBarOpenTime.time_since_epoch()).count();

    const auto fetch = m_restClient->fetchClosedKlines(
        symbol,
        interval,
        signalOpenMs,
        requiredClosedBars,
        std::chrono::seconds(m_config.runtimeRestFetchTimeoutSeconds),
        m_config.maxRestRequestsPerSignal);

    out.restPagesUsed = fetch.pagesUsed;
    out.restWallTimeMs = fetch.wallTime;

    if (!fetch.success) {
        out.errorReason = fetch.errorMessage.empty() ? "rest_failed" : fetch.errorMessage;
        return out;
    }

    out.availableBars = static_cast<int>(fetch.bars.size());
    if (out.availableBars < requiredClosedBars) {
        out.errorReason = "insufficient_history";
        return out;
    }
    if (fetch.bars.empty() || fetch.bars.back().openTime != signalOpenMs) {
        out.errorReason = "signal_bar_missing";
        return out;
    }
    if (!isSaneWindow(std::span<const Kline>(fetch.bars.data(), fetch.bars.size()))) {
        out.errorReason = "invalid_history";
        return out;
    }

    try {
        m_cache.merge(symbol, interval, std::span<const Kline>(fetch.bars.data(), fetch.bars.size()));
    } catch (...) {
        out.errorReason = "cache_writeback_failed";
    }

    out.sufficient = true;
    out.closedKlines.assign(fetch.bars.end() - requiredClosedBars, fetch.bars.end());
    return out;
}

}  // namespace backtest
