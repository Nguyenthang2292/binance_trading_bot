#include "backtest/rest_backfilling_historical_window_provider.h"

#include "scanner/kline_cache.h"

#include <chrono>
#include <span>
#include <utility>

namespace backtest {

RestBackfillingHistoricalWindowProvider::RestBackfillingHistoricalWindowProvider(
    std::unique_ptr<IHistoricalWindowProvider> innerProvider,
    std::unique_ptr<IKlineRestClient> restClient,
    scanner::KlineCache& cache,
    BacktestGateDataConfig config)
    : m_innerProvider(std::move(innerProvider)),
      m_restClient(std::move(restClient)),
      m_cache(cache),
      m_config(std::move(config)) {}

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
