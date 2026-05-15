# Elder's TSTS

Source: `The Universal Tactics of Successful Trend Trading_ Finding Opportunity in Uncertainty{Brent Penfold}(September 1 2020, Wiley){106182236} libgen.li_pages_191_260.md`

### *Elder's TSTS (1985)*

Dr Alexander Elder developed his strategy in 1985 and first published it in *Futures* magazine in 1986. He later shared the strategy in his bestselling book *Trading for a Living* (Wiley, 1993). The strategy uses multiple timeframes to find trend-trading opportunities. It specifically looks for a retracement against a higher timeframe trend before entering a trade that is aligned with the trend. The main takeaway is to operate over three timeframes. For most private traders that would be weekly, daily and intra-day. The week would define the trend, the daily would define the retracement level and intra-day would define the entry level.

Trend Elder uses the slope of the weekly MACD (moving average convergence divergence) histogram to define the trend. That is the relationship between the immediate weekly bar and the one prior. If the slope is up, the trend would be bullish and only buying opportunities would be considered. If the slope is down, the trend could be considered bearish and only selling opportunities would be considered. According to Elder, the best buy signals were the upward weekly MACD slopes that occurred below the centre line (negative numbers), while the best sell signals were the downward weekly MACD slopes that occurred above the centre line (positive numbers). On my testing I wasn't able to verify this so I didn't place any restrictions on defining the weekly trend except for the slope of the weekly MACD histogram.

---

224 THE UNIVERSAL TACTICS OF SUCCESSFUL TREND TRADING

**Retracement** To identify an appropriate retracement against the weekly trend Elder used either his own Force Index or Elder-Ray oscillator indicators. He also mentioned traders could consider using either a Stochastic or Williams %R to identify suitable retracement levels. For this review I've used Elder's own Elder-Ray oscillator indicator.

**Trade Plan** Elder enters on a break of the previous bar that satisfies both the trend and retracement condition. For buys an initial stop is placed at the lowest low of either the setup or entry bar. For sell trades an initial stop is placed at the highest high of either the setup or entry bar. The initial stop is adjusted to breakeven once an open profit appears. A 50% retracement of open profits is used as a trailing stop. Unfortunately, I found the strategy's trailing stop a little problematic. Certainly, the initial and break even stops are fine, and placing a trailing stop at the 50% retracement level of open profits appears sound and logical. However, the issue is practicality—where the strategy can keep a position open for multiple years, while the underlying market goes on a multiple year run. Some may feel that would be a strength of the strategy, to pick up unbelievable trades, and yes on a theoretical level it is, but on a practical level its nonsense. From my experience, traders find it difficult enough to hang on to a winning trade for three days let alone three weeks or three years. As a consequence, I've used the closest swing point as the trailing stop.

Here are the rules I'll use for my interpretation of Elder's TSTS.

### Rules

Strategy: TSTS
Developed: 1985
Published: 1986
Data: Daily
Approach: Trend trading
Technique: Retracement
Symmetry: Buy and sell
Markets: All
Indicators: MACD (12, 26, 9)
Elder-Ray (13)
Variables—Number: 5
MACD (3)
Elder-Ray (1)
Trailing Stop: Percentage (50%)
protection of open profit

---

Strategies 225

Variables—Symmetry: Same value for both buy and sell setups
Variables—Application: Same value across all markets
Rules: 6

### <u>Buy Rules</u>

Trend: Up—weekly MACD histogram bar is rising
Last weekly histogram bar is above previous weekly histogram bar
Retracement: Down—Elder-Ray Bear Power declines below zero & then ticks back up towards the centre line
Entry: Buy break of the previous bar’s high
Initial Stop: Sell break of the lowest low of either the setup or entry bar
Breakeven Stop: Move to breakeven at first open profit
Trailing Stop: Sell break of the closest swing low

### <u>Sell Rules</u>

Trend: Down—weekly MACD histogram bar is falling
Last weekly histogram bar is below previous weekly histogram bar
Retracement: Up—Elder-Ray Bull Power rallies above zero & then ticks back down towards the centre line
Entry: Sell a break of the previous bar’s low
Initial Stop: Buy break of the highest high of either the setup or entry bar
Breakeven Stop: Move to breakeven at first open profit
Trailing Stop: Buy break of closest swing high

In Figure 6.26 I've programmed TSTT to mechanically and sys-tematically identify and trade those trend-trading opportunities that satisfy the above rules.

Just a quick note on the chart in Figure 6.26. Those familiar with MACD will detect that the representation is not how the MACD is usu-ally shown. As I've mentioned, I do all my own programming in VBA

---

226 THE UNIVERSAL TACTICS OF SUCCESSFUL TREND TRADING

Elder Ray daily retracement up against the weekly MACD downtrend.
Exit Exit

1. Sell
2. Sell
3. Sell
Trailing stop at the closest swing point.
Rising weekly MACD Histogram implies the weekly trend is down.
Exit

**FIGURE 6.26** Elder's TSTS will wait for a retracement against a higher timeframe trend before initiating a trade.

for Excel. So, I’ve transposed the weekly MACD histogram into a daily representation that is shown here by the horizontal dashed line. When it appears above the daily bars it’s telling me that the weekly histogram bar for the last complete week is below the previous week’s histogram bar, or in other words, is falling. That is telling me, according to TSTT, that the weekly trend is down and TSTT can only look to sell after a retracement up has occurred as defined by the Elder-Ray indicator.

Here are TSTS’s performance results over my universal P24 portfolio since 1980.

<u>Results</u>
Portfolio P24: SB, ZW, CO, SO, HO, LC, GF, BP, SV,
KC, CT, ZB, GC, HG, JY, LH, SP, TY,
CL, FV, NG, ND, EC, YM
Start: 1980

Net Profit: $336,473
Total Trades: 11,633
Avg Profit: $29
Avg Brok & Slip per Trade: -$51

The good news is that the majority of these results are out-of-sample and are positive. The bad news is that the average net profit is

---

Strategies 227

very low. However, I feel the greatest value to be derived from Elder’s TSTS strategy is the important message to be in harmony with the higher timeframe trend. To know what the higher timeframe trend is and to ensure you’re aligned with it.
