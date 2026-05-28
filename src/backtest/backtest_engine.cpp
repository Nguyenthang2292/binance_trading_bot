/**
 * @file backtest_engine.cpp
 * @brief Deterministic fold backtest implementation used by gate scoring.
 */

#include "backtest/backtest_engine.h"

#include <cmath>
#include <limits>
#include <optional>

namespace backtest {

namespace {
/**
 * @brief Represents an open position tracked by the backtest engine.
 *
 * Holds direction, entry price, stop-loss and take-profit levels, and the
 * time the position was opened. This is an internal helper used by the
 * deterministic fold runner.
 */
struct OpenPosition {
    strategy::Signal::Direction direction{strategy::Signal::Direction::None};
    double entryPrice{0.0};
    double sl{0.0};
    double tp{0.0};
    int64_t openTimeMs{0};
};

/**
 * @brief Retrieve a numeric parameter value from a parameter map.
 *
 * Looks up `key` in `params` and returns the associated value if present,
 * otherwise returns the provided default `def`.
 *
 * @param params Parameter map (string -> double).
 * @param key Key to look up.
 * @param def Default value to return when the key is not found.
 * @return The parameter value from the map or `def` when missing.
 */
double getParam(const ParamPoint& params, const std::string& key, double def) {
    auto it = params.find(key);
    return it != params.end() ? it->second : def;
}

/**
 * @brief Direction to use when rounding a price to tick size.
 *
 * - `Down` rounds toward negative infinity (floor-like behavior).
 * - `Up` rounds toward positive infinity (ceil-like behavior).
 */
enum class TickRounding {
    Down,
    Up,
};

/**
 * @brief Round a raw price/value to the nearest tick multiple.
 *
 * Performs robust rounding using the specified `tickSize`. If `tickSize`
 * is non-positive or either input is non-finite the original `value` is
 * returned unchanged. The `rounding` parameter controls whether rounding
 * moves the value up or down to the nearest tick grid.
 *
 * @param value The price/value to round.
 * @param tickSize Tick increment to round to.
 * @param rounding Whether to round up or down.
 * @return The rounded value on success, otherwise the original `value`.
 */
double roundToTick(double value, double tickSize, TickRounding rounding) {
    if (tickSize <= 0.0 || !std::isfinite(tickSize) || value <= 0.0 || !std::isfinite(value)) {
        return value;
    }
    const double rawSteps = value / tickSize;
    const double epsilon = 1e-9;
    const double steps = rounding == TickRounding::Down
        ? std::floor(rawSteps + epsilon)
        : std::ceil(rawSteps - epsilon);
    const double rounded = steps * tickSize;
    return rounded > 0.0 && std::isfinite(rounded) ? rounded : value;
}

}  // namespace

BacktestStats BacktestEngine::runFold(
    const IOptimizableStrategy& adapter,
    std::string_view symbol,
    std::string_view interval,
    const std::vector<Kline>& foldKlines,
    const ParamPoint& params,
    const strategy::StrategyConfig& baseConfig,
    strategy::Signal::Direction acceptedDirection,
    const std::optional<ExchangeSymbol>& symbolMeta) const {

    /**
     * @brief Execute a deterministic fold backtest for a single symbol/interval.
     *
     * The runner uses the provided `adapter` to evaluate trade signals on an
     * expanding window of historical `foldKlines`. Signals are filtered by
     * `acceptedDirection` and `baseConfig.minConfidence`, then executed at the
     * open of the next bar. Positions use stop-loss and take-profit derived
     * from ATR and configured multipliers. Returns aggregated statistics for
     * the fold including counts, risk metrics and performance ratios.
     *
     * @param adapter Strategy adapter used to generate signals.
     * @param symbol Symbol under test.
     * @param interval Candle interval string (e.g. "1m", "1h").
     * @param foldKlines Kline sequence for the fold (time-ordered).
     * @param params Optimizable parameters supplied to the strategy.
     * @param baseConfig Base strategy config (leverage, fees, thresholds).
     * @param acceptedDirection Only signals matching this direction are traded.
     * @param symbolMeta Optional symbol metadata (tick size, etc.).
     * @return BacktestStats Aggregated statistics for the fold.
     */
    BacktestStats stats;
    if (foldKlines.size() < 2) return stats;

    const double slMult = getParam(params, "sl_multiplier", baseConfig.slMultiplier);
    const double tpMult = getParam(params, "tp_multiplier", baseConfig.tpMultiplier);
    const int leverage  = baseConfig.leverage > 0 ? baseConfig.leverage : 1;
    const double takeProfitPct = m_cfg.useFixedTakeProfit
        ? m_cfg.fixedTakeProfitPercent
        : baseConfig.takeProfitPercent;
    const bool useFixedTakeProfit = takeProfitPct > 0.0;
    const double tickSize = symbolMeta.has_value() ? symbolMeta->tickSize : 0.0;
    const int64_t maxHoldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        baseConfig.maxHoldDuration).count();

    std::vector<double> pnlPcts;
    std::optional<OpenPosition> pos;
    std::vector<Kline> growingWindow;
    growingWindow.reserve(foldKlines.size());

    double grossProfit = 0.0;
    double grossLoss = 0.0;
    double equity = 1.0;
    double peakEquity = 1.0;
    double maxDrawdown = 0.0;
    int wins = 0;

    for (std::size_t i = 0; i + 1 < foldKlines.size(); ++i) {
        growingWindow.push_back(foldKlines[i]);

        // ----- exit logic on next bar -----
        if (pos) {
            const auto& nextBar = foldKlines[i + 1];
            bool slHit = false, tpHit = false;

            if (pos->direction == strategy::Signal::Direction::Long) {
                if (nextBar.low  <= pos->sl) slHit = true;
                if (nextBar.high >= pos->tp) tpHit = true;
            } else {
                if (nextBar.high >= pos->sl) slHit = true;
                if (nextBar.low  <= pos->tp) tpHit = true;
            }

            bool exit = false;
            double exitPrice = 0.0;
            if (slHit) {                       // conservative: stop wins on collision
                exit = true;
                exitPrice = pos->sl;
            } else if (tpHit) {
                exit = true;
                exitPrice = pos->tp;
            } else if (maxHoldMs > 0 && (nextBar.closeTime - pos->openTimeMs) >= maxHoldMs) {
                exit = true;
                exitPrice = nextBar.close;
            }

            if (exit) {
                // Slippage on exit
                if (pos->direction == strategy::Signal::Direction::Long) {
                    exitPrice *= (1.0 - m_cfg.slippageBps / 10000.0);
                } else {
                    exitPrice *= (1.0 + m_cfg.slippageBps / 10000.0);
                }

                double raw = (pos->direction == strategy::Signal::Direction::Long)
                    ? (exitPrice - pos->entryPrice) / pos->entryPrice
                    : (pos->entryPrice - exitPrice) / pos->entryPrice;

                // Leverage + round-trip fees (entry + exit)
                double pnlPct = raw * leverage - m_cfg.takerFeeRate * leverage * 2.0;

                pnlPcts.push_back(pnlPct);
                stats.numTrades++;
                if (pnlPct > 0.0) { wins++; grossProfit += pnlPct; }
                else              { grossLoss   += -pnlPct; }

                equity *= (1.0 + pnlPct);
                if (equity > peakEquity) peakEquity = equity;
                const double dd = peakEquity > 0.0 ? (peakEquity - equity) / peakEquity : 0.0;
                if (dd > maxDrawdown) maxDrawdown = dd;

                pos.reset();
            }
        }

        // ----- entry logic at next bar open -----
        if (!pos) {
            const auto sig = adapter.evaluateWith(symbol, interval, growingWindow, params, baseConfig);
            if (sig.direction == strategy::Signal::Direction::None) continue;
            if (sig.confidence < baseConfig.minConfidence) continue;
            if (sig.atr <= 0.0) continue;
            if (sig.direction != acceptedDirection) continue;  // only trade signals matching the live direction

            const auto& nextBar = foldKlines[i + 1];
            double entry = nextBar.open;
            if (sig.direction == strategy::Signal::Direction::Long) {
                entry *= (1.0 + m_cfg.slippageBps / 10000.0);
            } else {
                entry *= (1.0 - m_cfg.slippageBps / 10000.0);
            }

            OpenPosition p;
            p.direction  = sig.direction;
            p.entryPrice = entry;
            p.openTimeMs = nextBar.openTime;

            if (sig.direction == strategy::Signal::Direction::Long) {
                p.sl = roundToTick(entry - slMult * sig.atr, tickSize, TickRounding::Up);
                p.tp = useFixedTakeProfit
                    ? entry * (1.0 + takeProfitPct / 100.0 / leverage)
                    : entry + tpMult * sig.atr;
                p.tp = roundToTick(p.tp, tickSize, TickRounding::Down);
            } else {
                p.sl = roundToTick(entry + slMult * sig.atr, tickSize, TickRounding::Down);
                p.tp = useFixedTakeProfit
                    ? entry * (1.0 - takeProfitPct / 100.0 / leverage)
                    : entry - tpMult * sig.atr;
                p.tp = roundToTick(p.tp, tickSize, TickRounding::Up);
            }
            pos = p;
        }
    }

    stats.sortino      = calculateSortino(pnlPcts);
    stats.sharpe       = calculateSharpe(pnlPcts);
    stats.maxDrawdown  = maxDrawdown;
    stats.profitFactor = grossLoss > 0.0 ? grossProfit / grossLoss
                                         : (grossProfit > 0.0 ? std::numeric_limits<double>::infinity() : 0.0);
    stats.winRate      = stats.numTrades > 0 ? static_cast<double>(wins) / stats.numTrades : 0.0;
    return stats;
}

/**
 * @brief Calculate the Sortino ratio for a series of P&L percentages.
 *
 * The Sortino ratio uses only downside volatility in the denominator. When
 * there are no downside observations a large positive mean maps to
 * +infinity while a non-positive mean yields 0.0.
 *
 * @param pnlPcts Vector of trade P&L percentages (fractional, e.g. 0.05 = 5%).
 * @return The Sortino ratio or `0.0`/`infinity` for degenerate cases.
 */
double BacktestEngine::calculateSortino(const std::vector<double>& pnlPcts) {
    if (pnlPcts.empty()) return 0.0;
    double sum = 0.0;
    for (double p : pnlPcts) sum += p;
    const double mean = sum / static_cast<double>(pnlPcts.size());

    double downsideSumSq = 0.0;
    int downsideN = 0;
    for (double p : pnlPcts) {
        if (p < 0.0) { downsideSumSq += p * p; downsideN++; }
    }
    if (downsideN == 0) return mean > 0.0 ? std::numeric_limits<double>::infinity() : 0.0;
    const double downsideStd = std::sqrt(downsideSumSq / static_cast<double>(downsideN));
    if (downsideStd == 0.0) return mean > 0.0 ? std::numeric_limits<double>::infinity() : 0.0;
    return mean / downsideStd;
}

/**
 * @brief Calculate the (sample) Sharpe-like ratio for a series of P&L percentages.
 *
 * This implementation computes mean / standard-deviation using the population
 * denominator (N). For degenerate cases where std == 0.0, a positive mean
 * maps to +infinity and non-positive mean yields 0.0.
 *
 * @param pnlPcts Vector of trade P&L percentages (fractional, e.g. 0.02 = 2%).
 * @return The Sharpe-like ratio or `0.0`/`infinity` for degenerate cases.
 */
double BacktestEngine::calculateSharpe(const std::vector<double>& pnlPcts) {
    if (pnlPcts.empty()) return 0.0;
    double sum = 0.0;
    for (double p : pnlPcts) sum += p;
    const double mean = sum / static_cast<double>(pnlPcts.size());
    double sumSq = 0.0;
    for (double p : pnlPcts) sumSq += (p - mean) * (p - mean);
    const double std = std::sqrt(sumSq / static_cast<double>(pnlPcts.size()));
    if (std == 0.0) return mean > 0.0 ? std::numeric_limits<double>::infinity() : 0.0;
    return mean / std;
}

}  // namespace backtest
