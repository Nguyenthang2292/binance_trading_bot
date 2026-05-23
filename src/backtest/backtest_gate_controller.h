#pragma once

#include "backtest/backtest_engine.h"
#include "backtest/backtest_gate.h"
#include "backtest/historical_window_provider.h"
#include "backtest/optimizable_strategy.h"
#include "backtest/range_proposer.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace backtest {

class BacktestGateController : public IBacktestGatePort {
public:
    BacktestGateController(
        IHistoricalWindowProvider& dataProvider,
        IRangeProposer& rangeProposer,
        std::unordered_map<std::string, std::unique_ptr<IOptimizableStrategy>> adapters,
        BacktestGateConfig cfg,
        BacktestEngine::Config engineCfg);

    BacktestGateResult evaluate(const BacktestGateRequest& req) const override;

private:
    IHistoricalWindowProvider& m_dataProvider;
    IRangeProposer& m_rangeProposer;
    std::unordered_map<std::string, std::unique_ptr<IOptimizableStrategy>> m_adapters;
    BacktestGateConfig m_cfg;
    BacktestEngine::Config m_engineCfg;
};

}  // namespace backtest
