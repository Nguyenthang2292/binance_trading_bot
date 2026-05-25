#pragma once

#include "backtest/backtest_gate.h"
#include "backtest/ihistorical_window_provider.h"
#include "backtest/ikline_rest_client.h"

#include <memory>

namespace scanner {
class KlineCache;
}

namespace backtest {

class RestBackfillingHistoricalWindowProvider final : public IHistoricalWindowProvider {
public:
    RestBackfillingHistoricalWindowProvider(
        std::unique_ptr<IHistoricalWindowProvider> innerProvider,
        std::unique_ptr<IKlineRestClient> restClient,
        scanner::KlineCache& cache,
        BacktestGateDataConfig config);

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
