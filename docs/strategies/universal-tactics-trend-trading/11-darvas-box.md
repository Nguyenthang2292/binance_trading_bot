# Darvas Box

Source: `The Universal Tactics of Successful Trend Trading_ Finding Opportunity in Uncertainty{Brent Penfold}(September 1 2020, Wiley){106182236} libgen.li_pages_191_260.md`

### The Darvas Box (1950)

The Darvas Box strategy is another congestion breakout strategy. Nicolas Darvas would (mentally) draw a box around congested prices to encapsulate range bound activity. He would enter the market on a break of the box.

Despite his book, Darvas did not specifically detail the exact rules for his method. He didn't specifically define the size or parameters of his box or his stop, except to infer he'd exit if the opposite box boundary was broken. He never actually drew any boxes on his charts, referring to himself as a mental chartist. In a nut shell, Darvas was looking for a congestion in prices, or consolidation. If prices broke out, he went long. In addition, Darvas was vague about the overall market condition he preferred to trade in. He made references to only examining shares trading at their all-time highs, and knowing a share's high and low over the preceding two or three years (making references to 52-week highs).

So, I need to define some hard rules if I hope to program and review the strategy. Now, although like many of his predecessors and contemporaries he used more than just price in his strategy (i.e. volume), it's my preference to keep my observations about these strategies I'm discussing both simple and consistent, so I'll just focus on price.

---

Strategies 205

I’ve summarized the rules according to my interpretation of the Darvas Box Strategy.

### Rules

Strategy: Darvas Box
Developed: 1950
Published: 1960
Data: Daily
Approach: Trend trading
Technique: Congestion breakout
Symmetry: Buy and sell
Markets: All
Indicators: Average True Range (ATR)
Variables—Number: 5
Darvas Box (4):
Minimum length of box: (20) daily bars
Maximum length of box: (100) daily bars
Maximum height of box: as defined by a multiple (5) of the ATR (20)
Trailing Stop: Number of weeks (2)
Variables—Symmetry: Same value for both buy and sell setups
Variables—Application: Same value across all markets
Rules: 4

### Buy Rules

Setup: Darvas Box
Trend: Up—previous close must be above the previous year’s high
Entry: Buy a break of the Darvas Box’s high
Stop: Sell break of the 2-week low

### Sell Rules

Setup: Darvas Box
Trend: Down—previous close must be below the previous year’s low
Entry: Sell a break of the Darvas Box’s low
Stop: Buy break of the 2-week high

I’ve programmed my interpretation of the Darvas Box strategy as illustrated in Figure 6.19.

---

206 THE UNIVERSAL TACTICS OF SUCCESSFUL TREND TRADING

**FIGURE 6.19** Nicolas Darvas used a mental box to define a congestion of prices. The Darvas strategy would trade a breakout of the congestion box.

Let's see how the Darvas Box strategy has performed over my P24 portfolio since 1980?

<u>Results</u>

Portfolio P24: SB, ZW, CO, SO, HO, LC, GF, BP, SV,  
KC, CT, ZB, GC, HG, JY, LH, SP, TY,  
CL, FV, NG, ND, EC, YM  
Start: 1980

Net Profit: $136,731  
Total Trades: 636  
Avg Profit: $215  
Avg Brok & Slip per Trade: -$51

Not bad compared to Livermore’s Reaction Model. But not great compared to the others. The positive news is that for a 70+year-old strategy it’s profitable. My only concern is the number of variables the strategy has. I’ve done no more than stipulate a minimum and maximum length for the box along with a maximum height. I’ve no doubt that if I changed those variables you’d see another alternative equity curve and resultant expectancy, ROR and average profit calculation.

---

Strategies 207

How alternative? I don’t know. But it’s a consideration you need to be aware of. In addition, I can’t say the results are out-of-sample as I’ve made up the variable values, not Nicolas Darvas. I’ve certainly utilized his ‘box’ philosophy, but the coded variables are mine. But, even with these reservations, the Darvas Box philosophy does enjoy an edge, and—along with many of the strategies above—validates the power of following the three golden tenets of trend trading.

This brings me to the last of the congestion-type breakout strategies I’ll be discussing: Curtis Arnold’s Pattern Probability Strategy.
