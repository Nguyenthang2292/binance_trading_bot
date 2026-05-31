/**
 * @file backtest_engine.h
 * @brief Deterministic in-memory fold backtester used by the backtest gate.
 */

#pragma once

#include "backtest/optimizable_strategy.h"
#include "backtest/parameter_space.h"
#include "strategy/strategy_config.h"
#include "types/market.h"

#include <chrono>
#include <optional>
#include <string_view>
#include <vector>

namespace backtest {

/**
 * @brief Summary statistics produced from one fold simulation.
 */
struct BacktestStats {
    int    numTrades{0};   ///< Number of completed trades.
    double sortino{0.0};   ///< Non-annualized sortino ratio.
    double sharpe{0.0};    ///< Non-annualized sharpe ratio.
    double profitFactor{0.0};   ///< Gross profit / gross loss.
    double maxDrawdown{0.0};    ///< Maximum equity drawdown over fold.
    double winRate{0.0};        ///< Fraction of profitable trades.
};

// In-memory backtest mirroring the live execution semantics that matter for
// fair per-combo comparison: next-bar entry, ATR-based SL/TP, fees, conservative
// entry-candle and same-candle SL+TP ordering, fixed-TP-percent path,
// tick-size SL/TP quantization (Long: SL up / TP down; Short: SL down /
// TP up), max-hold time-exit, and fold-end mark-to-market.
//
// No RNG. Identical inputs MUST produce identical outputs.
class BacktestEngine {
public:
    /**
     * @brief Runtime assumptions for fill and fee modeling.
     */
    struct Config {
        double takerFeeRate{0.0004};   ///< Per-side taker fee rate.
        double slippageBps{0.0};       ///< Entry/exit slippage in basis points.
        bool   useFixedTakeProfit{false};   ///< If true, ignore tp_multiplier in params.
        double fixedTakeProfitPercent{0.0}; ///< Fixed TP percentage when override is enabled.
    };

    explicit BacktestEngine(Config cfg) : m_cfg(std::move(cfg)) {}

    /**
     * @brief Simulates one walk-forward fold for a single parameter point.
     *
     * Entry uses next-bar open after a valid signal, exits honor SL/TP/time,
     * and results are fully deterministic for identical inputs.
     */
    BacktestStats runFold(
        const IOptimizableStrategy& adapter,
        std::string_view symbol,
        std::string_view interval,
        const std::vector<Kline>& foldKlines,
        const ParamPoint& params,
        const strategy::StrategyConfig& baseConfig,
        strategy::Signal::Direction acceptedDirection,
        const std::optional<ExchangeSymbol>& symbolMeta = std::nullopt) const;

private:
    static double calculateSortino(const std::vector<double>& pnlPcts);
    static double calculateSharpe(const std::vector<double>& pnlPcts);

    Config m_cfg;
};

}  // namespace backtest
