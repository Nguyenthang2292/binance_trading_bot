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

struct BacktestStats {
    int    numTrades{0};
    double sortino{0.0};        // non-annualized
    double sharpe{0.0};         // non-annualized
    double profitFactor{0.0};
    double maxDrawdown{0.0};
    double winRate{0.0};
};

// In-memory backtest mirroring the live execution semantics that matter for
// fair per-combo comparison: next-bar entry, ATR-based SL/TP, fees, conservative
// same-candle SL+TP ordering, fixed-TP-percent path, tick-size SL/TP
// quantization (Long: SL up / TP down; Short: SL down / TP up), max-hold
// time-exit.
//
// No RNG. Identical inputs MUST produce identical outputs.
class BacktestEngine {
public:
    struct Config {
        double takerFeeRate{0.0004};
        double slippageBps{0.0};
        bool   useFixedTakeProfit{false};   // when true, ignore tp_multiplier; use fixedTakeProfitPercent
        double fixedTakeProfitPercent{0.0};
    };

    explicit BacktestEngine(Config cfg) : m_cfg(std::move(cfg)) {}

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
