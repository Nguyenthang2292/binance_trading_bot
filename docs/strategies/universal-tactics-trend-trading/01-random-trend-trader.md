# Random Trend Trader

Source: `The Universal Tactics of Successful Trend Trading_ Finding Opportunity in Uncertainty{Brent Penfold}(September 1 2020, Wiley){106182236} libgen.li_pages_191_260.md`

## Trading Naked Without Technical Analysis

We now know hard scientific data tells us trend trading can't lose. The existence of fat tails and their regular appearance demonstrates that following the trend is a proven strategy to earn outsize returns.

I can demonstrate this with a simple strategy I'll call Random Trend Trader. The strategy will use Excel's random number generator, or coin toss, to signal either a buy or sell trade on the open. The model will hold a position without a stop for one day and then exit the following day on the open.

Here are the rules.

### Rules

| Field | Value |
| :--- | :--- |
| Strategy | Random Trend Trader |
| Setup | None |
| Entry | Random coin toss entry, either buying or selling on the open |
| Stop | No stop |
| Exit | Exit next day market on open |
| Brokerage | None |
| Portfolio P8 | Japanese Yen, five-year treasury notes, e-mini Nasdaq, natural gas, copper, soybeans, coffee, lean hogs |

Figure 6.1 shows the equity curve.

**Random Trend Trader - 8 Market Portfolio**  
Random Entry, No Stop, Exit Next Day Open, No Brokerage.

**Figure 6.1** Random Trend Trader uses a coin toss to generate buy and sell signals.

Over the eight-market portfolio the strategy hypothetically made:

### Results

| Field | Value |
| :--- | :--- |
| Portfolio P8 | SB, LC, GC, CO, TY, SP, CL & EC |
| Start | 1980 |
| Profit | $264,429 |
| Trades | 64,686 |
| Avg Profit | $4 |
| Avg Brok & Slip per Trade | $0 |

Although a profitable strategy, you'll notice it would quickly become unprofitable once brokerage and slippage is added. However, for the purpose of this exercise it will do. For a strategy, with a random entry, it's not bad. Now let's look at a histogram of its results in Figure 6.2.

You can see the distribution of results is almost normal, with individual trades being constant through time, falling symmetrically either side of an average value, and with half the trades positive and the other half negative. Where it's not "normal" is in the fat tails. This is where Random Trend Trader can take advantage of both the science and the three golden tenets to improve its results. Let's first look to see if cutting losses short will benefit our strategy.

**Figure 6.2** Random Trend Trader's histogram of individual trade results.

## Golden Tenet: Cut Your Losses Short

We can see from the histogram in Figure 6.2 that there are a significant number of large losses. If we can add say a 1% stop to our strategy to embrace the "cut your losses short" golden tenet we should be able to chop off the large negative fat tails and instantly improve Random Trend Trader's profitability.

Figure 6.3 shows Random Trend Trader's new equity curve after the 1% stop has been added.

Over the eight-market portfolio the revised strategy made:

### Results

| Field | Value |
| :--- | :--- |
| Portfolio P8 | SB, LC, GC, CO, TY, SP, CL & EC |
| Start | 1980 |
| Profit | $243,121 |
| Trades | 64,686 |
| Avg Profit | $3.76 |
| Avg Brok & Slip per Trade | $0 |

**Random Trend Trader - 8 Market Portfolio**  
Random Entry, 1% Stop, Exit Next Day Open, No Brokerage.

**Figure 6.3** Random Trend Trader's performance after the introduction of a 1% stop.

Unfortunately, cutting losses short with the 1% stop doesn't appear to have improved performance with the average profit dropping to $3.76, or has it? Let's see if the histogram of individual trades in Figure 6.4 can show us more.

Well, how about that. Although the average profit has dropped, what the equity curve hasn't shown is the reduction in the large negative fat tails. Yes, the net profit is lower, however, Random Trend Trader has earned less profit with *far fewer* large negative losses, making it more comfortable to trade. So, one benefit of cutting your losses short is that it does make a strategy easier to trade. Let's now see if letting profits run will also benefit Random Trend Trader.

**Figure 6.4** Introducing a 1% stop reduced Random Trend Trader's large losses.

## Golden Tenet: Letting Your Profits Run

Building on Random Trend Trader, we'll now look to add a trailing stop loss. Adding to the initial 1% stop Random Trend Trader will no longer exit on the next day's open, but will stay in a winning position until the previous week's low (for longs) or high (for shorts) is broken.

Figure 6.5 shows Random Trend Trader's new hypothetical equity curve once a trailing stop has been added to the initial 1% stop.

**Random Trend Trader - 8 Market Portfolio**  
Random Entry, 1% Stop, Exit Previous Week High/Low, No Brokerage.

**Figure 6.5** Random Trend Trader's performance after introducing a trailing one-week stop.

Wow, what an improvement it makes letting your profits run. Maybe this tenet should be called the "golden, golden" tenet! Over the eight-market portfolio the new revised strategy hypothetically made:

### Results

| Field | Value |
| :--- | :--- |
| Portfolio P8 | SB, LC, GC, CO, TY, SP, CL & EC |
| Start | 1980 |
| Profit | $618,000 |
| Trades | 10,958 |
| Avg Profit | $56.40 |
| Avg Brok & Slip per Trade | $0 |

Let's see if Figure 6.6's histogram of individual trades can give us any additional insights?

The histogram clearly shows the benefit gained from the golden, golden tenet with the huge jump in extreme "positive" fat tails. Letting profits run has shifted many of the positive results to the extreme edge where the big profitable trades are.

**Figure 6.6** Introducing a one-week trailing stop increased the number of Random Trend Trader's large profits.

## Random Trend Trader

Well, there you have it. A "naked" strategy designed to avoid negative fat tails and to benefit from positive fat tails. A profitable trading strategy I've developed without even utilizing one piece of technical analysis. A profitable trading strategy with a random coin toss for its entry signal.

Let's recap the strategy's revised rules:

### Rules

| Field | Value |
| :--- | :--- |
| Strategy | Random Trend Trader |
| Setup | None |
| Entry | Random coin toss entry, either buying or selling on the open |
| Initial Stop | 1% |
| Trailing Stop | A break of the previous week's high for shorts and low for longs |
| Brokerage | None |

### Results

| Field | Value |
| :--- | :--- |
| Portfolio P8 | SB, LC, GC, CO, TY, SP, CL & EC |
| Start | 1980 |
| Profit | $618,000 |
| Trades | 10,958 |
| Avg Profit | $56.40 |
| Avg Brok & Slip per Trade | $0 |

Now, we can't get too excited as the profitability is marginal. If you deduct $50 for brokerage and slippage the average profit falls to $6.40 per trade.

However, the point here is not to suggest that Random Trend Trader is a preferred strategy to trade but to demonstrate how the hard science of mathematics proves following the trend is a proven strategy that can't lose. Random Trend Trader proves, even with a random and nonsensical entry technique, that just cutting losses short and letting profits run is a desirable and profitable approach to trading. You can't argue against the math, it is as inviolable as the laws of gravity!

But wait. Do I hear a few suspicious whispers suggesting I may be a little guilty of data mining since I've only selected eight markets? Have I just shown you the best eight markets to prove a point? Am I no different to those rainbow merchants who promote trading as the proverbial money tree offering effortless and boundless riches? No, I don't think so. So, let me run Random Trend Trader over my universal P24 portfolio, which contains 24 markets, being the three most liquid markets in each of eight diverse market segments. Figure 6.7 shows the results.

Results over the P24 Portfolio are as follows.

### P24 Results

| Field | Value |
| :--- | :--- |
| Portfolio P24 | SB, ZW, CO, SO, HO, LC, GF, BP, SV, KC, CT, ZB, GC, HG, JY, LH, SP, TY, CL, FV, NG, ND, EC, YM |
| Start | 1980 |
| Profit | $1,567,646 |
| Trades | 31,953 |
| Avg Profit | $49 |
| Avg Brok & Slip per Trade | $0 |

**Random Trend Trader - 24 Market Portfolio**  
Random Entry, 1% Stop, Exit Previous Week High/Low, No Brokerage.

**Figure 6.7** Random Trend Trader's performance on the Universal P24 Portfolio.

Well how about that. Although it would be a marginal proposition if brokerage and slippage were included, the model's performance based on a random coin toss and adhering to two golden tenets over a larger and well diversified portfolio, covering the most liquid markets in their segments, is outstanding. Didn't I say as inviolable as the laws of gravity? The math doesn't lie. Fat tails exist. Cutting losses short and letting profits run is a proven strategy 200-years young. A strategy with a nonsensical random coin toss entry without even a hint of technical analysis. It's all math.

Now that our little scientific experiment is complete, it's time to look at a number of different trend-trading strategies. Each using their preferred elements of technical analysis to embrace the three golden tenets of trend trading: follow the trend, cut losses short and let profits run.
