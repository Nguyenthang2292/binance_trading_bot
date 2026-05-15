# Gartley's Three- and Six-Week Crossover

Source: `The Universal Tactics of Successful Trend Trading_ Finding Opportunity in Uncertainty{Brent Penfold}(September 1 2020, Wiley){106182236} libgen.li_pages_191_260.md`

### *Gartley's Three- and Six-Week Crossover (1935)*

Gartley is probably better known for his Gartley 1–2–3 retracement chart pattern. A pattern made popular by Larry Pesavento, who applied harmonic ratios. Gartley is not known for his dual moving average strategy unless you have a copy of his 1935 book *Profits in the Stock Market*. On page 266 you will see the chart illustration shown in Figure 6.10.

---

Strategies 183

<u>FIGURE 8</u>

MOVING AVERAGE STUDY
OF N. Y. TIMES AVERAGE OF 50 STOCKS
SHOWING
3 AND 6 WEEK MOVING AVERAGES
OF HIGH, LOW & MEAN

**FIGURE 6.10** Gartley clearly illustrated his three and six week crossover strategy in his 1935 book *Profits in the Stock Market*.

Let me summarize the rules.

### Rules

| | |
| :--- | :--- |
| Strategy: | Gartley 3 and 6 week crossover |
| Developed: | Unknown |
| Published: | 1935 |
| Data: | Daily |
| Approach: | Trend trading |
| Technique: | Always in the market: a stop and reverse<br>relative price rate of change |
| Symmetry: | Buy and sell |
| Markets: | All |
| Indicators: | Moving average (x 3) |
| Variables—Number: | 3<br>Short-term Moving average:<br>High/Low<br>3-week moving average of the weekly<br>mean |

---

184 THE UNIVERSAL TACTICS OF SUCCESSFUL TREND TRADING

Mean is average of the weekly
High/Low
Longer-term Moving Average 1:
6-week moving average of the weekly
high
Offset (moved forward) by 2-weeks
Longer-term Moving Average 2:
6-week moving average of the weekly
low
Offset (moved forward) by 2-weeks
Weekly offset (2)

Variables—Symmetry: Same value for both buy and sell setups
Variables—Application: Same value across all markets

### Buy Rules

Setup: 3-week moving average of mean > 6-week moving average of the highs
Entry: Buy Monday market on open
Stop: 3-week moving average of mean < 6-week moving average of the lows
Sell Monday Market on open

### Sell Rules

Setup: 3-week moving average of mean < 6-week moving average of the lows
Entry: Sell Monday market on open
Stop: 3-week moving average of mean > 6-week moving average of the highs
Buy Monday market on open

Like the Hearne model, I've programmed Gartley's strategy into my VBA Excel trading model according to his rules as shown in Figure 6.11.

Being a relative momentum strategy the Gartley model watches for when prices are above or below previous prices. In Gartley's case, he uses three price series. When the three-week moving average of the mean closes on a Friday above the six-week moving average of the high (which is offset by two weeks), the model will go long on Monday's open. When the three-week moving average of the mean closes on Friday below the six-week moving average of the lows (which is offset by two weeks) the model will go short on Monday's open.

---

Strategies 185

**FIGURE 6.11** Gartley incorporated moving average calculations way before they became popular and way before the arrival of personal computers in the 1970s.

Let's see how his model, first published in 1935, has performed over my P24 portfolio since 1980?

### Results

Portfolio P24: SB, ZW, CO, SO, HO, LC, GF, BP, SV, KC, CT, ZB, GC, HG, JY, LH, SP, TY, CL, FV, NG, ND, EC, YM
Start: 1980
Net Profit: $1,079,398
Total Trades: 3,387
Avg Profit: $319
Avg Brok & Slip per Trade: -$51

Well how about that. A strategy over 80 years old holding up exceedingly well over the last 40-odd years on out-of-sample data. Harold Gartley, take a bow! Straight off the pages from his 1935 book. Here is a trend-trading strategy that not only proves the golden tenets of trend trading work but also provides rock solid evidence of the methodology's robustness over 40+ years of out-of-sample data. Everyone, please stand and applaud Mr Harold Gartley.

---

186 THE UNIVERSAL TACTICS OF SUCCESSFUL TREND TRADING
