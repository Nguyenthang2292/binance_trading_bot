# Ricardo Rules

Source: `The Universal Tactics of Successful Trend Trading_ Finding Opportunity in Uncertainty{Brent Penfold}(September 1 2020, Wiley){106182236} libgen.li_pages_191_260.md`

### *Ricardo Rules (1800)*

James Grant wrote about David Ricardo in his 1838 book *The Great Metropolis, Volume 2*:

> *I may observe that he amassed his immense fortune by a scrupulous attention to what he called his own three golden rules, the observance of which he used to press on his private friends. These were,*
>
> * *Never refuse an option when you get it*
> * *Cut your losses short*
> * *Let your profits run on.*

---

## Strategy Implementation

Let me formulate a strategy to encapsulate these three golden rules.

#### Never Refuse an Option When You Get It

To capture Ricardo's belief to 'never refuse an option', I'll use the market's price action as the 'gift' of direction that should 'never' be refused. Simply:

* If there is no position and the market breaks a previous bar's high, Ricardo Rules should say 'go long'
* If the market breaks a previous bar's low, the Ricardo Rules should say 'go short'

A trader should listen to the market. Its direction is the 'gift' a trader should 'never' refuse.

#### Cut Your Losses Short

Let's keep it simple. For an initial stop, I'll use an opposite break of either the setup or entry bar, whichever one is further away:

* If prices break up (taking out the previous setup bar's high), the Ricardo Rules will place an initial stop one tick below the lowest low of either setup bar or entry bar
* If prices break down (taking out the previous setup bar's low), the Ricardo Rules will place an initial stop one tick above the highest high of either the setup bar or entry bar

#### Let Your Profits Run On

If the market takes off, I'll use the closest swing point as a trailing stop:

* If the Ricardo Rules go long with prices rallying, a trailing stop will be raised to one tick below the nearest swing low
* If the Ricardo Rules go short with prices falling, a trailing stop will be lowered to one tick above the nearest swing high

Let me now summarize my interpretation of Ricardo Rules.

## Rules Summary

| Attribute | Value |
|-----------|-------|
| Strategy | Ricardo Rules |
| Developed | 1800 |
| Published | 1838 |
| Data | Daily |
| Approach | Trend trading |
| Technique | Price breakout |
| Symmetry | Buy and sell |
| Markets | All |
| Indicators | None |
| Variables | 0 |
| Rules | 3 |

### Buy Rules

| Element | Rule |
|---------|------|
| Setup | Neutral daily bar |
| Entry | Buy break of the previous bar's high |
| Initial Stop | Sell break of the lowest low of either the setup or entry bar |
| Trailing Stop | Sell break of the closest swing low |

### Sell Rules

| Element | Rule |
|---------|------|
| Setup | Neutral daily bar |
| Entry | Sell break of the previous bar's low |
| Initial Stop | Buy break of the highest high of either the setup or entry bar |
| Trailing Stop | Buy break of the closest swing high |

## Example Trade Illustration

I've programmed the strategy into my VBA Excel trading model where the illustration below shows a buy trade according to the rules.

**Figure 6.16: Sample Trade Structure**

The price levels illustrate a typical price sequence:

```
5473
5373
5273
5173
5073
4973
```

**Trade Steps:**

1. Stopped out - Neutral setup bar
2. First breakout to the upside - **Buy**
3. Initial Stop - Lowest low of either the setup or entry bar
4. Trailing Stop - Closest swing point
5. Exit executed

The Ricardo Rules strategy will follow the first daily bar breakout and remain in a position until the break of either its initial or trailing stop.

## Performance Results

This simple representation of David Ricardo's approach to trading, used by him in the 1800s, reflects his core philosophy:
* Accept all gifts of market direction (never refuse an option)
* Cut your losses short
* Let your profits run

### Backtest Results: Portfolio P24

**Portfolio Composition:** SB, ZW, CO, SO, HO, LC, GF, BP, SV, KC, CT, ZB, GC, HG, JY, LH, SP, TY, CL, FV, NG, ND, EC, YM

| Metric | Value |
|--------|-------|
| Start Date | 1980 |
| Net Profit | $622,552 |
| Total Trades | 20,392 |
| Average Profit per Trade | $31 |
| Average Broker & Slippage per Trade | -$51 |

### Analysis

While not a world-beater, the Ricardo Rules strategy demonstrates solid performance. The average profit of $31 per trade is modest; however, considering the simplicity of the model, the overall profitability is impressive. This robustness is a clear testament to the power of trend trading's three golden principles.
