# Monthly Close Model

Source: `The Universal Tactics of Successful Trend Trading_ Finding Opportunity in Uncertainty{Brent Penfold}(September 1 2020, Wiley){106182236} libgen.li_pages_191_260.md`

### *Monthly Close Model (1933)*

This model will simply enter long if the current month closes above the previous month's close. It will reverse and go short if the current month closes below the previous month's close. The strategy is always in the market and is referred to as a stop and reverse strategy.

---

Strategies        191

I’ve summarized the rules here.

### Rules

| | |
| :--- | :--- |
| Strategy: | Monthly Close Model |
| Developed: | 1933 |
| Published: | 1933 |
| Data: | Daily |
| Approach: | Trend trading |
| Technique: | Always in the market: a stop and reverse<br>relative time rate of change |
| Symmetry: | Buy and sell |
| Markets: | All |
| Indicators: | None |
| Variables—Number: | 0 |
| Variables—Symmetry: | Not applicable |
| Variables—Application: | Not applicable |
| Rules: | 2 |

### Buy Rules

| | |
| :--- | :--- |
| Setup: | Current monthly close > previous<br>monthly close |
| Entry: | Buy next day, first day of the month,<br>market on open |
| Stop: | Current monthly close < previous<br>monthly close<br>Sell next day, first day of the month,<br>market on open |

### Sell Rules

| | |
| :--- | :--- |
| Setup: | Current monthly close < previous<br>monthly close |
| Entry: | Sell next day, first day of the month,<br>market on open |
| Stop: | Current monthly close > previous<br>monthly close<br>Buy next day, first day of the month,<br>market on open |

While a monthly close continues in a trade’s direction, the model will let profits run but will cut and reverse the position if a monthly close reverses.

---

192 THE UNIVERSAL TACTICS OF SUCCESSFUL TREND TRADING

1. Close below previous month's close. **Sell**.
2. Close above previous month's close. **Buy**.
**Buy**
Previous Month's Close
**Sell**
3. Close below previous month's close. **Sell**.

**FIGURE 6.14** The Monthly Close Model strategy will stop and reverse positions following an opposite directional monthly close.

Like the other models, I've programmed this simple relative time model according to the rules above as shown in Figure 6.14.

Being a relative time momentum strategy the model watches for where the end-of-month price closes relative to the previous monthly close. If the model is short and the monthly close is up, the model will stop and reverse and go long on the open of the first day of the next month. If the model is long and prices close down the model will stop and reverse and go short on the open of the first day of the next month

Let's see how his model has performed over my P24 portfolio since 1980?

### Results

Portfolio P24: SB, ZW, CO, SO, HO, LC, GF, BP, SV,
KC, CT, ZB, GC, HG, JY, LH, SP, TY,
CL, FV, NG, ND, EC, YM
Start: 1980

Net Profit: $1,003,526
Total Trades: 4,993
Avg Profit: $201
Avg Brok & Slip per Trade: -$51

---

Strategies 193

Well how about that? Not bad for a simplistic model that waits for time and let's winning trades run, while cutting and reversing when losing. Again, this demonstrates the power of trend trading's three golden tenets. Cowles and Jones, hold hands and take a well-deserved bow.
