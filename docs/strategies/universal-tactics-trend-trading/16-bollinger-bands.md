# Bollinger Bands

Source: `The Universal Tactics of Successful Trend Trading_ Finding Opportunity in Uncertainty{Brent Penfold}(September 1 2020, Wiley){106182236} libgen.li_pages_191_260.md`

### *Bollinger Bands (1993)*

John Bollinger created his indicator in the 1980s. Bollinger Bands consist of three bands. A middle, upper and lower band. The bands contain two variables: the number of days, or length of period, and the number of standard deviations used to offset the upper and lower bands beyond the middle band. The middle band represents the moving average of the period in question while the upper and lower bands represent the standard deviation of prices away from the middle moving average value. When prices are range bound, rotating back and forth, the upper and lower standard deviation bands compress to reflect the lower volatility. When prices are directional and moving the upper and lower bands expand to reflect the higher volatility.

Band width is determined by the number of standard deviations the upper and lower bands are placed beyond the middle value.

If the upper and lower bands are drawn one standard deviation from the middle band, then one would expect prices to move within the upper and lower bands 68% of the time. Consequently, when prices close outside the bands it's a rare 32% occurrence and is possibly an indication of a new trend commencing.

If the upper and lower bands are drawn two standard deviations from the middle band then one would expect prices to move within the upper and lower bands 95% of the time. Consequently, when

---

218 THE UNIVERSAL TACTICS OF SUCCESSFUL TREND TRADING

prices close outside the bands it's a very, very rare 5% occurrence and is possibly a strong indication of a new trend commencing.
So, the number of standard deviations used to offset the outer bands is an important variable. Certainly, the more standard deviations used the rarer the occurrence is of an outside close beyond the bands, and possibly a stronger indication of any potential trend. However, the downside is that the rarer the outside close is, the fewer trading opportunities will be presented.
Anyway, the idea is that a close outside the upper or lower bands is seen as an indication of a new trend commencing. So entries occur when a daily close occurs outside the bands while a stop is placed at an opposite close beyond the middle band.
Now, there are a number of various trading strategies that utilize Bollinger Bands. Both trend and counter-trend. Unfortunately, there isn't one standout *publicized* trend-trading Bollinger Band strategy that I can program and review and say, '*here are the out-of-sample results*'. Although Bollinger Bands have been around since the 1980s, a publicized model with clearly defined variable values does not.
However, having said that, there was a very popular and successful commercially available strategy based on Bollinger Bands that was developed in 1986 and first sold in 1993. The strategy was named as 'One of the Top 10 Trading Systems of All Time' by *Futures Truth* magazine. I never bought the strategy so I don't know what the variable values are, and even if I had, I wouldn't share them here as it would be a breach of trust. However, my point here is to let you know that even though there is no publicized strategy using Bollinger Bands I'm aware of that I can review and show out-of-sample results on, I can share with you that there was a very well-known and successful trend-trading strategy based on Bollinger Bands that was first sold in 1993.
Consequently I've used '1986' as the development date and '1993' as the 'release date', not to say my Bollinger Band strategy has been released since 1993, but to acknowledge Bollinger Bands as being the backbone of one of the most popular and successful trend-trading strategies from the 1990s.
With that being said, I'll program a trend-trading Bollinger Bands strategy using 80 days and one standard deviation. Here are the rules I'll use.

### Rules

Strategy:                 Bollinger Bands
Developed:                1986
Published:                1993

---

Strategies 219

Data: Daily
Approach: Trend trading
Technique: Volatility breakout
Symmetry: Buy and sell
Markets: All
Indicators: Bollinger Bands
Variables—Number: 2
Bollinger Bands (80)
Standard deviation multiplier (1) used to create the upper and lower bands
Variables—Symmetry: Same value for both buy and sell setups
Variables—Application: Same value across all markets
Rules: 2

### Buy Rules

Trend: Up—previous close above the upper Bollinger band
Entry: Buy next day market on open
Stop: Previous close below the middle Bollinger band
Sell next day market on open

### Sell Rules

Trend: Down—previous close below the lower Bollinger band
Entry: Sell next day market on open
Stop: Previous close above the middle Bollinger band
Buy next day market on open

In Figure 6.24 I've programmed my Bollinger Bands strategy to mechanically and systematically identify trading opportunities according to the rules above.

Let's see how the Bollinger Band strategy has performed over my P24 portfolio.

### Results

Portfolio P24: SB, ZW, CO, SO, HO, LC, GF, BP, SV, KC, CT, ZB, GC, HG, JY, LH, SP, TY, CL, FV, NG, ND, EC, YM
Start: 1980

---

220 THE UNIVERSAL TACTICS OF SUCCESSFUL TREND TRADING

1. Close above the upper Bollinger Band, **Buy** on open next day.
2. Close below the middle Bollinger Band, **Exit** on open next day.
3. Close above the upper Bollinger Band, **Buy** on open next day.
4. Close below the middle Bollinger Band, **Exit** on open next day.

Lower Band: MA(80) - 1 x Std Dev (80)
Middle Band: Moving Average (80)
Upper Band: MA(80) + 1 x Std Dev (80)

**FIGURE 6.24** The Bollinger Band strategy will initiate positions following an expansion of prices beyond a one standard deviation movement, as defined by prices closing outside the upper and lower bands.

Net Profit: $1,558,476  
Total Trades: 2,954  
Avg Profit: $528  
Avg Brok & Slip per Trade: –$51  

Wacko. Very good I'd say! Unfortunately, I can't say these results are out-of-sample, since I've made up the variable values. And consequently, I can't say these results demonstrate the robustness of the strategy. However, I think it's only fair to say well done to John Bollinger for developing a tool that uses the power of science to monitor and identify trend-trading opportunities. A tool used by a very popular and successful strategy that was first commercially sold in 1993. A strategy finding trading opportunities that embrace and celebrate the three golden tenets of trend trading.
