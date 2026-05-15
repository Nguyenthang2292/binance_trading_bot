# Livermore Reaction

Source: `The Universal Tactics of Successful Trend Trading_ Finding Opportunity in Uncertainty{Brent Penfold}(September 1 2020, Wiley){106182236} libgen.li_pages_191_260.md`

### *Livermore Reaction (1900)*

The Livermore Reaction model is the first of the congestion-type breakout models I'll be reviewing. Jess Livermore may just be the

---

Strategies 201

most celebrated trader I know of. I know Edwin Lefèvre's 1923 book *Reminiscences of a Stock Operator* had a profound impact on myself. I can still remember reading the book as a young trader when I was on Bank America's securities desk in Sydney, and believing he was referring to myself when he was categorizing all of his mistakes. I was thinking, 'that's me', 'yes, that's me, that's me!'. If you haven't got yourself a copy, do so now.

Jesse Livermore was a trend trader who summarized how he traded in his 1940 book *How to Trade in Stocks*. In his book he makes direct reference to following the trend:

*It may surprise many to know that in my method of trading, when I see by my records that an upward trend is in progress, I become a buyer as soon as a stock makes a new high on its movement, after having had a normal reaction. The same applies whenever I take the short side. Why? Because I am following the trend at the time. My records signal me to go ahead!*

Livermore defined his 'normal reactions' as two pullbacks, or retracements, against the new trend. He used the term 'pivots' to describe swing points and defined trends according to the position of his 'pivots' or swing points. He defined an uptrend as being characterized by higher pivot (swing) highs and higher pivot (swing) lows. He defined a downtrend as being characterized by lower pivot (swing) highs and lower pivot (swing) lows. Since this is identical to Dow's peak-and-trough trend analysis I'll use the term 'Dow Theory'.

Livermore would look for a change in trend to occur (as defined by Dow Theory), wait patiently while two normal reactions (or retracements) occurred against the new trend, before entering on a break of the previous swing point, which would also reconfirm the new trend. He would stay in the trade until a change in Dow trend would stop him out. A change in Dow trend was simply an opposite break of previous pivot or swing point.

It's impossible to determine an accurate year for when he developed his approach. I'll assume he would have developed and traded it by his early to mid- twenties, so I'll haphazard a guess and say 1900.

Livermore gave consideration to other factors in his trading, however, in the interest of keeping it simple and comparable to the other strategies discussed here, my preference is to keep my discussion on his trading strategy to price alone.

---

202 THE UNIVERSAL TACTICS OF SUCCESSFUL TREND TRADING

I’ve summarized the rules below here.

### Rules

| | |
| :--- | :--- |
| Strategy: | Livermore Reaction |
| Developed: | 1900 |
| Published: | 1940 |
| Data: | Daily |
| Approach: | Trend trading |
| Technique: | Congestion breakout |
| Symmetry: | Buy and sell |
| Markets: | All |
| Indicators: | None |
| Variables—Number: | 0 |
| Variables—Symmetry: | Not applicable |
| Variables—Application: | Not applicable |
| Rules: | 4 |

### Buy Rules

| | |
| :--- | :--- |
| Setup: | Change in Dow trend from trend down to trend up<br>Two reactions/retracements swing down against the new Dow uptrend |
| Entry: | Buy break of the previous swing high |
| Stop: | Sell break of the closest swing low |

### Sell Rules

| | |
| :--- | :--- |
| Setup: | Change in Dow trend from trend up to trend down<br>Two reactions/retracements swing up against the new Dow down trend |
| Entry: | Sell break of the previous swing low |
| Stop: | Buy break of the closest swing high |

I’ve programmed Livermore’s Reaction strategy into my VBA Excel trading model as illustrated in Figure 6.18.

Let’s see how Jesse Livermore’s Reaction strategy, programmed as a mechanical systemized model, has performed over my P24 portfolio since 1980?

---

Strategies 203

* 1. Break of previous swing high. Change of trend. Trend is up.
* Previous swing high
* 1. 1st Reaction (pullback).
* 1. 2nd Reaction (pullback).
* Previous swing low
* 1. Break of previous swing high. Trend confirmation. **Buy**.
* 1. Break of previous swing low. **Exit** position.

**FIGURE 6.18** The Livermore Reaction strategy will only initiate a trade following a 'normal reaction' as defined by two retracements against a new trend.

### Results

| | |
|---|---|
| Portfolio P24: | SB, ZW, CO, SO, HO, LC, GF, BP, SV,<br>KC, CT, ZB, GC, HG, JY, LH, SP, TY,<br>CL, FV, NG, ND, EC, YM |
| Start: | 1980 |
| Net Profit: | $35,136 |
| Total Trades: | 1,279 |
| Avg Profit | $27 |
| Avg Brok & Slip per Trade: | -$51 |

Well that is disappointing. Given it's the renowned Jesse Livermore I was expecting (hoping) for more. But not to be. A positive is that it's at least profitable on out-of-sample data, demonstrating the core idea is robust. Not robust enough to trade as it is, but possibly robust enough to build upon by an energetic and enthusiastic trader. If that's you, remember to tread carefully and avoid falling into the trap of excessive curve fitting. Keep your focus on capturing market signals and not market noise.

---

204 THE UNIVERSAL TACTICS OF SUCCESSFUL TREND TRADING

As an aside it's interesting to note that Livermore's Reaction method is at direct odds with Ralph Elliott's Wave Theory. Elliott developed his approach in the 1930s and called it the Elliott Wave Theory. In its simplest form Elliott believed a trend is complete following a five-wave movement. Yet Livermore's Reaction is looking to enter the market on a fifth wave, just when Elliott is expecting the market to reverse. I know I can be accused of oversimplifying Elliott Wave, as there are many rules governing wave relationships and fractal layering; however, my observation is still valid. One approach is looking for a trend to continue, while the other is looking for a trend to reverse. They are diametrically opposed to each other. Interesting, hey? And it also gives a good insight into the difficulties that traders have in sorting out what works and what doesn't within the field of technical analysis. There are so many competing and opposing voices. Really, one could be forgiven for thinking technical analysis sometimes resembles a mad house!
