# Trend Breakout Trader

**Type:** `trend_breakout`  
**Version:** 1.1  
**Date:** 2026-05-15  
**Design doc:** [docs/design/strategies/2026-05-15-strategy-trend-breakout-v1.0.md](../design/strategies/2026-05-15-strategy-trend-breakout-v1.0.md)

---

## Tổng Quan

Trend Breakout Trader là trend-following strategy lấy cảm hứng từ *Random Trend Trader* (Brent Penfold, 2020). Strategy trade **Donchian breakout 20 candle trên timeframe 4H**: close của candle 4H đã đóng gần nhất vượt khỏi high/low của 20 candle 4H liền trước.

Exit chính là generic engine trailing stop qua `TrailingStopController`; plugin không tự giữ trailing state.

---

## Điều Kiện Vào Lệnh

| Direction | Điều kiện |
|---|---|
| Long | `interval == "4h"` và close của candle đã đóng gần nhất > high cao nhất của 20 candle 4H liền trước |
| Short | `interval == "4h"` và close của candle đã đóng gần nhất < low thấp nhất của 20 candle 4H liền trước |

Plugin bỏ qua mọi interval khác `4h`.

---

## Tham Số Mặc Định

| Param | Giá trị | Ý nghĩa |
|---|---|---|
| `intervals` | `["4h"]` | Timeframe scan chính |
| `breakout_period` | 20 | Lookback Donchian Channel trên 4H |
| `atr_period` | 14 | Chu kỳ ATR |
| `risk_pct` | 0.01 | 1% balance mỗi lệnh |
| `sl_multiplier` | 1.5 | Initial SL = entry ± 1.5 × ATR |
| `tp_multiplier` | 20.0 | TP xa để trailing stop là exit chính |
| `trailing_enabled` | true | Bật generic engine trailing stop |
| `trailing_interval` | `4h` | Interval dùng để tính trailing |
| `trailing_candles` | 42 | Lowest low / highest high của 42 candle 4H đã đóng |

---

## Runtime Notes

- Signal dùng candle đã đóng để tránh bias.
- Engine mở market order ở giá hiện hành khi xử lý signal, không đảm bảo fill tại close của candle signal.
- `TrailingStopController` chỉ dùng candle đã đóng và chỉ move stop theo hướng có lợi.
- Cần backtest với fee, slippage, và entry model tương ứng trước khi bật live.
