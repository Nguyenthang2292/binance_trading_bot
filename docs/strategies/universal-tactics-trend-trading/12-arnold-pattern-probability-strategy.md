# Arnold's Pattern Probability Strategy

Source: `The Universal Tactics of Successful Trend Trading_ Finding Opportunity in Uncertainty{Brent Penfold}(September 1 2020, Wiley){106182236} libgen.li_pages_191_260.md`

### *Arnold’s Pattern Probability Strategy (1987)*

Curtis Arnold developed his Pattern Probability Strategy (PPS) in 1987 and later published it in his book *PPS Trading System* (Arnold, 1995). Unfortunately for Arnold, in 1997 he ran into trouble with the US Commodity Futures Trading Commission. He made a few claims they objected to. However, despite his misstep, I believe his PPS strategy is worth reviewing.

PPS is a very simple strategy looking to trade breakouts of price congestions in the direction of a medium-term and long-term trend as defined by the 18-day and 40-day moving average.

Price congestions are defined by traditional chart patterns such as triangles, rectangles, wedges, head and shoulders and double and triple tops and bottoms. For simplicity I’ll focus on the triangle, rectangle and wedge patterns.

Arnold originally looked for patterns that contained at least ten days, and no more than 50 days. In his book he indicates that patterns with fewer than ten days may also be worth considering. So, it appears he’s flexible on pattern size. For this exercise I’ll focus purely on pattern and ignore any minimum or maximum number of days or bars.

PPS uses a combination of initial, breakeven and trailing stops. For its initial stop PPS uses the apex of the two converging connected trend lines. For simplicity I’ll use an opposite break of either the setup or entry bar, depending on which is furthest away. PPS will move its initial stop to breakeven once the open profit is twice the initial risk or if on the fourth day there is an open profit. PPS uses two trailing stops. A break of the closest swing point or a break of a 45-degree trend line. For simplicity I’ll stick with only using a break of the closest swing point.

---

208 THE UNIVERSAL TACTICS OF SUCCESSFUL TREND TRADING

Here are the rules according to my interpretations.

### Rules

Strategy: PPS
Developed: 1987
Published: 1995
Data: Daily
Approach: Trend trading
Technique: Congestion breakout
Symmetry: Buy and sell
Markets: All
Indicators: Moving average (× 2)
Variables—Number: 5
Medium-term trend: Moving average (18)
Long-term trend: Moving average (40)
Breakeven stop: Open profit multiple (2) of trade risk
Breakeven stop: Minimum open profit after minimum number of days (4)
Number of swing points required to locate a setup pattern (4)
Note: Each pair of swings is connected by a trend line
Breakeven Stop: Open profit multiple (2) of trade risk
Breakeven Stop: Minimum open profit after minimum number of days (4)
Number of swing points required to locate a setup pattern (4)
Note: Each pair of swings is connected by a trend line
Variables—Symmetry: Same value for both buy and sell setups
Variables—Application: Same value across all markets
Rules: 6

### Buy Rules

Setup: Chart pattern (Triangles, rectangles and wedges)
Trend: Up—medium-term 18-day moving average is rising while the long-term 40-day moving average is either flat or rising.
Entry: Buy break of pattern’s top trend line

---

Strategies 209

**Initial Stop:** Sell break of the lowest low of either the setup or entry bar

**Breakeven**
**Stop:** Move stop to breakeven at the first of either:

1. Open profit is greater than twice the risk.
2. Open profit exists after 4 days.

**Trailing Stop:** Sell break of the closest swing low

### Sell Rules

**Setup:** Chart pattern (Triangles, rectangles and wedges)

**Trend:** Down—medium-term 18-day moving average is falling while the long-term 40-day moving average is either flat or falling.

**Entry:** Sell break of pattern's bottom trend line

**Initial Stop:** Buy break of the highest high of either the setup or entry bar

**Breakeven**
**Stop:** Move stop to breakeven at the first of either:

1. Open profit is greater than twice the risk.
2. Open profit exists after 4 days.

**Trailing Stop:** Buy break of the closest swing high

I've programmed PPS into my VBA Excel trading model, according to my interpretation of the rules, to mechanically and systematically locate and trade breakouts of congestion patterns that are aligned with the underlying trend, as illustrated in Figure 6.20.

Let's see how my interpretation of Arnold's PPS strategy has performed over my P24 portfolio since 1980?

### Results

**Portfolio P24:** SB, ZW, CO, SO, HO, LC, GF, BP, SV, KC, CT, ZB, GC, HG, JY, LH, SP, TY, CL, FV, NG, ND, EC, YM

**Start** 1980

**Net Profit:** $450,780
**Total Trades:** 2,586
**Avg Profit:** $174
**Avg Brok & Slip per Trade:** -$51

---

210 THE UNIVERSAL TACTICS OF SUCCESSFUL TREND TRADING

**FIGURE 6.20** The PPS strategy looks to trade breakouts off traditional congestion patterns that are aligned with the trend.

Well, pretty good actually. We should also acknowledge Robert Edwards and John Magee with a hat tip for their help in popularizing congestion patterns in their seminal book *Technical Analysis of Stock Trends* (Martino Publishing, 1948). Arnold, despite his run in with the US Commodity Futures Trading Commission, has enveloped congestion patterns within a simple and logical trade plan. A trade plan whose performance, over a majority of out-of-sample data, not only demonstrates PPS's robustness but also helps validate the merits of trend trading.
