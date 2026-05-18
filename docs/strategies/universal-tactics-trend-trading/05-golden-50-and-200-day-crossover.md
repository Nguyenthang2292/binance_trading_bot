# Golden 50 and 200-Day Crossover

**Source:** [The Universal Tactics of Successful Trend Trading: Finding Opportunity in Uncertainty](https://www.wiley.com/) - Brent Penfold, September 1 2020

## Overview

I'm not aware that this strategy has been attributed to any individual trader. However, due to its frequent appearance in the media and regular referencing by market analysts, it's worth reviewing.

You may have seen references to a "Golden Cross" or "Death Cross" occurring in a particular market. Many market participants believe that the appearance of either signal represents a seismic change in trend.

- **Golden Cross:** A 50-day moving average crosses above a 200-day moving average, suggesting a bull market
- **Death Cross:** A 50-day moving average crosses below a 200-day moving average, suggesting a bear market

## Strategy Parameters

| Parameter | Value |
|-----------|-------|
| Strategy Name | Golden 50 and 200-day Crossover |
| Developer | Unknown |
| Published | Unknown |
| Data Frequency | Daily |
| Approach | Trend trading |
| Technique | Stop and reverse; relative price rate of change |
| Symmetry | Buy and sell (symmetric) |
| Markets | All |
| Indicators | Moving average (2 variants) |
| Number of Variables | 2 |
| Medium-term Trend | 50-day moving average |
| Long-term Trend | 200-day moving average |
| Variable Symmetry | Same value for buy and sell setups |
| Application | Same value across all markets |
| Total Rules | 2 |

## Buy Rules

| Element | Rule |
|---------|------|
| **Setup** | 50-day MA > 200-day MA |
| **Entry** | Buy at market on next day's open |
| **Stop** | 50-day MA < 200-day MA → Sell at market on next day's open |

## Sell Rules

| Element | Rule |
|---------|------|
| **Setup** | 50-day MA < 200-day MA |
| **Entry** | Sell at market on next day's open |
| **Stop** | 50-day MA > 200-day MA → Buy at market on next day's open |

## Historical Context

This strategy is very similar to Richard Donchian's 5 and 20-day crossover methodology. The only difference is in the length of the respective moving averages. The strategy was programmed into a VBA Excel trading model to test performance.

## Backtest Results

### Portfolio P24

**Symbols tested:** SB, ZW, CO, SO, HO, LC, GF, BP, SV, KC, CT, ZB, GC, HG, JY, LH, SP, TY, CL, FV, NG, ND, EC, YM

**Test Period:** 1980 - Present

| Metric | Value |
|--------|-------|
| Net Profit | $1,715,940 |
| Total Trades | 1,235 |
| Average Profit per Trade | $1,389 |
| Average Broker/Slippage per Trade | -$51 |

## Analysis

The strategy demonstrates excellent performance with a high average profit of $1,389, making it highly consistent. The Golden Crossover strategy exemplifies the principle of following the trend, cutting losses short, and letting profits run.

The key to the strategy's success is its disciplined approach to trend following and systematic exit rules that capture significant market moves while minimizing drawdowns.

## Next Steps

This analysis demonstrates the effectiveness of trend-following strategies. The next exploration should examine relative time rate of change strategies and how they compare to moving average crossover approaches.
