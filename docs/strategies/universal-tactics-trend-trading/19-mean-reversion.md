# Mean Reversion

Source: `The Universal Tactics of Successful Trend Trading_ Finding Opportunity in Uncertainty{Brent Penfold}(September 1 2020, Wiley){106182236} libgen.li_pages_191_260.md`

### *Mean Reversion (2020)*

Similar to TSTT, this strategy will wait for a retracement against the underlying trend before looking to initiate a trade in the direction of the trend. This strategy looks to take advantage of the market’s tendency to revert to its mean. Markets will regularly move in one direction, suggesting further movement ahead, before inevitably reversing back. Much like the effect of stretching a rubber band, which will always snap back to its original position. The existence of ‘thin peaks’ demonstrates this tendency. I don’t have a name for this strategy except to refer to it as mean reversion. I’ll use two Bollinger Bands channels with one standard deviation. I’ll use a longer length (30-day) Bollinger Band to define the trend and a shorter (15-day) Bollinger Band to define the retracement. The strategy will wait for a daily close outside the longer (30-day) Bollinger Band channel to identify the trend. It will then wait for an opposite daily close outside the shorter (15-day) Bollinger Band channel to signify that a retracement has occurred. Once a retracement has been identified, a trade will be initiated in direction of the trend at the first break of a daily bar. An initial stop will be placed at the opposite break of either the setup or entry bar, whichever is furthest away, while a trailing stop will be placed at the closest swing point. Similar to the ATR Band strategy, I can’t attribute this methodology to any particular trader with confidence, so for conservatism I’ll assume its existence began from the present.

Let me summarize the rules below.

<u>Rules</u>

Strategy: Mean Reversion
Developed: 2020
Published: 2020
Data: Daily
Approach: Trend trading
Technique: Retracement
Symmetry: Buy and sell

---

228 THE UNIVERSAL TACTICS OF SUCCESSFUL TREND TRADING

Markets: All
Indicators: Bollinger Bands
Variables—Number: 3
Length of the Bollinger Band trend channel (30)
Length of the Bollinger Band retracement channel (15)
Standard deviation multiplier (1) used to create the upper and lower bands
Variables—Symmetry: Same value for both buy and sell setups
Variables—Application: Same value across all markets
Rules: 5

### Buy Rules

Trend: Up—a previous close is above the trend upper Bollinger band
Retracement: Down—previous close is below the retracement lower Bollinger band
Entry: Buy first break of the previous bar’s high
Initial Stop: Sell break of the lowest low of either the setup or entry bar
Trailing Stop: Sell break of the closest swing low

### Sell Rules

Trend: Down—a previous close is below the trend lower Bollinger band
Retracement: Up—previous close is above the retracement upper Bollinger band
Entry: Sell first break of the previous bar’s low
Initial Stop: Buy break of the highest high of either the setup or entry bar
Trailing Stop: Buy break of the closest swing high

---

Strategies        229

**[Nội dung biểu đồ]**

1. TREND: Clse < Trend BB Channel. Trend down.
2. RETRACEMENT: Clse > Retrace. BB Channel. Retracement up.
   3. Entry: First break of previous low. **Sell.**
   4. Initial Stop: Stopped out.

3. TREND: Clse > Trend BB Channel. Trend up.
4. RETRACEMENT: Clse < Retrace. BB Channel. Retracement down.
   7. Entry: First break of previous high. **Buy.**
   8. Trailing Stop: Exit at break of closest swing low.

*Các chú thích trên biểu đồ:*
* Trend BB Channel
* Retracement BB Channel
* Trailing Stop: Closest swing low

**FIGURE 6.27** The mean reversion strategy will wait for a retracement against the trend before initiating a trade.

In Figure 6.27 I've programmed this mean reversion strategy to mechanically and systematically identify trading opportunities according to the rules above.

Below are the strategy's hypothetical performance results over my universal P24 portfolio since 1980.

### <u>Results</u>

| | |
|---|---|
| Portfolio P24: | SB, ZW, CO, SO, HO, LC, GF, BP, SV,<br>KC, CT, ZB, GC, HG, JY, LH, SP, TY,<br>CL, FV, NG, ND, EC, YM |
| Start: | 1980 |
| Net Profit: | $535,005 |
| Total Trades: | 5,163 |
| Avg Profit: | $104 |
| Avg Brok & Slip Per Trade: | -$51 |

Despite its good performance, I can't say these results demonstrate the robustness of the strategy as they're not out-of-sample. I've only just created this strategy along with the variable values. However,

---

230 THE UNIVERSAL TACTICS OF SUCCESSFUL TREND TRADING

I think the strategy presents a good demonstration of how a mean reversion retracement trend-trading strategy works. A strategy that finds trading opportunities that embrace and celebrate the three golden tenets of trend trading.
