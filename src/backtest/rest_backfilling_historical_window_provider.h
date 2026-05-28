/**
 * @file rest_backfilling_historical_window_provider.h
 * @brief Historical window provider with cache-first then REST backfill behavior.
 */

#pragma once

#include "backtest/backtest_gate.h"
#include "backtest/ihistorical_window_provider.h"
#include "backtest/ikline_rest_client.h"

#include <memory>

namespace scanner {
class KlineCache;
}

namespace backtest {

/**
 * @brief Decorator provider that backfills cache misses using REST.
 */
class RestBackfillingHistoricalWindowProvider final : public IHistoricalWindowProvider {
public:
    /**
     * @brief Constructs provider with cache-only inner source and REST adapter.
     */
    RestBackfillingHistoricalWindowProvider(
        std::unique_ptr<IHistoricalWindowProvider> innerProvider,
        std::unique_ptr<IKlineRestClient> restClient,
        scanner::KlineCache& cache,
        BacktestGateDataConfig config);

    /**
     * @brief Returns required closed bars, backfilling from REST when needed.
     */
    WindowResult closedWindow(
        std::string_view symbol,
        std::string_view interval,
        int requiredClosedBars,
        std::chrono::system_clock::time_point signalBarOpenTime) const override;

private:
    std::unique_ptr<IHistoricalWindowProvider> m_innerProvider;
    std::unique_ptr<IKlineRestClient> m_restClient;
    scanner::KlineCache& m_cache;
    BacktestGateDataConfig m_config;
};

}  // namespace backtest
