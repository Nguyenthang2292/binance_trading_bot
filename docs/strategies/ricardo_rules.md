# Ricardo Rules

**Type:** `ricardo_rules`
**Version:** 1.0
**Date:** 2026-05-19
**Source ref:** `penfold-2020-universal-tactics` — Chapter 7, "Ricardo Rules"
**Design doc:** [docs/design/2026-05-19-strategy-ricardo_rules-v1.0.md](../design/2026-05-19-strategy-ricardo_rules-v1.0.md)

---

## Tổng Quan

Ricardo Rules hiện thực hóa ba nguyên tắc giao dịch của David Ricardo (1800): vào lệnh khi thị trường phá vỡ bar trước (không bao giờ từ chối cơ hội), cắt lỗ bằng stop tại extreme của setup/entry bar, và để lợi nhuận chạy bằng swing-point trailing stop. Strategy là pure price-action breakout — không có indicator nào ngoài ATR phục vụ sizing/confidence. Để chạy đúng nguyên bản, engine phải hỗ trợ custom initial stop, swing trailing thật, và exit policy không có fixed take-profit.

---

## Điều Kiện Vào Lệnh

| Direction | Điều kiện |
|---|---|
| **Long** | Bar vừa đóng (`klines[-2]`) có `close > high` của bar trước đó (`klines[-3]`) |
| **Short** | Bar vừa đóng (`klines[-2]`) có `close < low` của bar trước đó (`klines[-3]`) |

> `klines[-1]` là bar đang hình thành — không dùng để tránh false signal.

---

## Indicators Sử Dụng

| Indicator | Params | Vai trò |
|---|---|---|
| ATR | `period = 14`, tính trên candles đã đóng | Đo volatility cho sizing/confidence; không dùng candle đang hình thành |

---

## Tham Số Mặc Định

| Param | Giá trị | Ý nghĩa |
|---|---|---|
| `intervals` | `["1d", "4h", "1h", "30m"]` | Timeframes scan |
| `atr_period` | 14 | Chu kỳ ATR |
| `risk_pct` | 0.01 | 1% balance mỗi lệnh |
| `sl_multiplier` | 1.5 | Legacy fallback only; Ricardo exact dùng custom initial stop |
| Binance Futures PNL/ROI% (`takeProfitPercent`) | 0.0 / disabled | Không đặt fixed TP để lợi nhuận chạy theo swing trailing |
| `tp_multiplier` | 0.0 / disabled | Không dùng ATR TP fallback khi engine hỗ trợ no-fixed-TP |
| `min_confidence` | 0.0 | Không lọc breakout hợp lệ; confidence chỉ dùng telemetry/ranking |
| `scan_interval_seconds` | 900 | Rescan mỗi 15 phút |
| `max_hold_duration_seconds` | 86400 | Time-exit sau 24h |
| `initial_stop_policy` | `setup_entry_extreme` | Long: 1 tick dưới lowest low setup/entry; Short: 1 tick trên highest high setup/entry |
| `exit_policy` | `swing_trailing` | Thoát bằng initial stop, swing trailing stop, hoặc time-exit |
| `swing_lookback` | 3 | N bars mỗi bên để xác nhận swing point |

---

## Điều Kiện Thị Trường Phù Hợp

Hoạt động tốt nhất trong **trending market** rõ ràng — khi giá liên tục tạo higher highs (uptrend) hoặc lower lows (downtrend). Kém hiệu quả trong sideways/ranging market vì breakout liên tục bị false — bar liên tục phá vỡ high/low của bar trước nhưng không duy trì được hướng.

---

## Giới Hạn & Cảnh Báo

- **Phụ thuộc engine extension**: Không enable production cho Ricardo exact cho tới khi engine hỗ trợ custom initial stop, swing-point trailing stop, và no-fixed-TP exit policy
- **Nhiều false signal trong ranging market**: Strategy không có filter trend, sẽ vào lệnh trên mọi breakout bar kể cả khi không có xu hướng
- **4 timeframes song song**: Cùng 1 symbol có thể tạo signal trên nhiều TF cùng lúc — WorkQueue zip scheduler quyết định TF nào được execute trong từng cycle
- Params mặc định chưa được backtest — cần optimize trước khi dùng production
