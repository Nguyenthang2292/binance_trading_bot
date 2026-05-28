/**
 * @file backtest_gate_controller.h
 * @brief Orchestrator implementing the full backtest-gate decision pipeline.
 */

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

/**
 * @brief Concrete gate controller coordinating data fetch, range proposal,
 *        walk-forward scoring, plateau selection, and final vote.
 */
class BacktestGateController : public IBacktestGatePort {
public:
    /**
     * @brief Constructs a gate controller with injected data/proposer/adapters.
     */
    BacktestGateController(
        IHistoricalWindowProvider& dataProvider,
        IRangeProposer& rangeProposer,
        std::unordered_map<std::string, std::unique_ptr<IOptimizableStrategy>> adapters,
        BacktestGateConfig cfg,
        BacktestEngine::Config engineCfg);

    /**
     * @brief Evaluates a signal candidate and returns pass/drop result.
     */
    BacktestGateResult evaluate(const BacktestGateRequest& req) const override;

private:
    IHistoricalWindowProvider& m_dataProvider;
    IRangeProposer& m_rangeProposer;
    std::unordered_map<std::string, std::unique_ptr<IOptimizableStrategy>> m_adapters;
    BacktestGateConfig m_cfg;
    BacktestEngine::Config m_engineCfg;
};

}  // namespace backtest
