# ATR Bands

Source: `The Universal Tactics of Successful Trend Trading_ Finding Opportunity in Uncertainty{Brent Penfold}(September 1 2020, Wiley){106182236} libgen.li_pages_191_260.md`

### ATR Bands (2020)

The other volatility breakout strategy I want to discuss is one based on ATR bands. It's identical to the Bollinger Bands strategy except that it uses the ATR, rather than the standard deviation, to measure volatility and create the upper and lower bands. Unfortunately, I can't attribute this strategy to any single trader with confidence.

---

Strategies 221

Here are the rules I've programmed.

### Rules

Strategy: ATR Bands
Developed: 2020
Published: 2020
Data: Daily
Approach: Trend trading
Technique: Volatility breakout
Symmetry: Buy and sell
Markets: All
Indicators: Moving Average
ATR
Variables—Number: 3
Moving Average (80)
ATR (80)
ATR Multiplier (2) used to create the upper and lower bands
Variables—Symmetry: Same value for both buy and sell setups
Variables—Application: Same value across all markets
Rules: 2

### Buy Rules

Trend: Up—previous close above the upper ATR band
Entry: Buy next day market on open
Stop: Previous close below the moving average
Sell next day market on open

### Sell Rules

Trend: Down—previous close below the lower ATR band
Entry: Sell next day market on open
Stop: Previous close above the moving average
Buy next day market on open

Like the Bollinger Band breakout strategy, I've programmed the ATR breakout strategy in line with the rules above, as shown in Figure 6.25.

---

222 THE UNIVERSAL TACTICS OF SUCCESSFUL TREND TRADING

**1.** Close above the ATR Upper Band, **Buy** next day on open.
**2.** Close below the ATR Middle Band, **Exit** next day on open.

*Upper Band: MA(80) plus 2 x ATR(80)*
*Middle Band: Moving Average (80)*
*Lower Band: MA(80) less 2 x ATR(80)*

**3.** Close below the ATR Lower Band, **Sell** next day on open.
**4.** Close above the ATR Middle Band, **Exit** next day on open.

**FIGURE 6.25** The ATR Bands strategy will initiate positions following an expansion of prices beyond a two average true range movement.

Let me now run the strategy over my universal P24 portfolio.

### Results

Portfolio P24: SB, ZW, CO, SO, HO, LC, GF, BP, SV, KC, CT, ZB, GC, HG, JY, LH, SP, TY, CL, FV, NG, ND, EC, YM
Start: 1980

Net Profit: $1,193,319
Total Trades: 3,544
Avg Profit: $337
Avg Brok & Slip per Trade: -$51

It’s also a winner. However, like the Bollinger Band strategy, these results don’t demonstrate the strategy’s robustness, as the figures are not out-of-sample because I made up the variable values. However, I’ve included it here alongside Bollinger Bands as an alternative measure of volatility.

That finishes my review of the absolute momentum ‘breakout’ trend-trading strategies. The other absolute momentum-type methodologies I want to look at are the ‘retracement’-type strategies.

---

Strategies 223
