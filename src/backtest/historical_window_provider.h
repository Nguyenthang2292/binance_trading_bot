/**
 * @file historical_window_provider.h
 * @brief Cache-backed implementation of historical closed-window retrieval.
 */

#pragma once

#include "backtest/backtest_gate.h"
#include "backtest/ihistorical_window_provider.h"


namespace scanner {
class KlineCache;
}

namespace backtest {

// Concrete cache-backed provider.
// `cache_only`: returns klines only when KlineCache holds enough closed bars.
// `cache_then_rest`: this class remains cache-only and is used as the inner
// provider by RestBackfillingHistoricalWindowProvider.
/**
 * @brief Reads closed kline windows from scanner cache.
 */
class HistoricalWindowProvider : public IHistoricalWindowProvider {
public:
    /**
     * @brief Builds a provider with read-only cache access and data config.
     */
    HistoricalWindowProvider(
        const scanner::KlineCache& cache,
        BacktestGateDataConfig config);

    /**
     * @brief Returns the latest required closed bars ending at signal bar T.
     */
    WindowResult closedWindow(
        std::string_view symbol,
        std::string_view interval,
        int requiredClosedBars,
        std::chrono::system_clock::time_point signalBarOpenTime) const override;

private:
    const scanner::KlineCache& m_cache;
    BacktestGateDataConfig m_config;
};

}  // namespace backtest
