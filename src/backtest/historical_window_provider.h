#pragma once

#include "backtest/backtest_gate.h"
#include "backtest/ihistorical_window_provider.h"


namespace scanner {
class KlineCache;
}

namespace backtest {

// Concrete cache-backed provider.
// `cache_only`: returns klines only when KlineCache holds enough closed bars.
// `cache_then_rest`: not yet implemented in v1.1. Startup validation disables
// the gate instead of letting this provider silently degrade in live use.
class HistoricalWindowProvider : public IHistoricalWindowProvider {
public:
    HistoricalWindowProvider(
        const scanner::KlineCache& cache,
        BacktestGateDataConfig config);

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
