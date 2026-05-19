# Monthly Close Model (Adapted MTF)

**Type:** `monthly_close_model`
**Version:** 1.1
**Date:** 2026-05-17
**Source ref:** `penfold-2020-universal-tactics` — Chapter 7, "Monthly Close Model"
**Design doc:** [docs/archives/design/2026-05-17-strategy-monthly_close_model-v1.0.md](../archives/design/2026-05-17-strategy-monthly_close_model-v1.0.md)

---

## Tổng Quan

Monthly Close Model (1933, Brent Penfold) là strategy momentum thuần túy: so sánh close của period hiện tại với period trước đó. Nếu giá close cao hơn → Long; thấp hơn → Short. Không có indicator, chỉ thuần so sánh giá — khai thác edge *trend persistence* (xu hướng có xu hướng tiếp tục).

Bot adaptation: dùng **fixed 30-candle period** trên mọi TF thay vì monthly calendar — cùng logic, khác time horizon. Engine chỉ giữ một tracked position cho mỗi symbol, nên TF đầu tiên mở được lệnh sẽ dominate các TF sau. Interval priority: `4h → 1h → 30m → 1d`.

---

## Điều Kiện Vào Lệnh

| Direction | Điều kiện |
|---|---|
| **Long** | Close hiện tại > close 30 candles trước (period close tăng) |
| **Short** | Close hiện tại < close 30 candles trước (period close giảm) |

**Confidence** = `|ΔClose| / prev_close` — độ mạnh của price move, clamped về `[0.0, 1.0]`.

---

## Indicators Sử Dụng

| Indicator | Params | Vai trò |
|---|---|---|
| ATR | period=14 | Đo volatility — engine dùng để sizing, SL và TP fallback theo `tp_multiplier` |

Không có indicator signal. Chỉ so sánh raw closing price.

---

## Tham Số Mặc Định

| Param | Giá trị | Ý nghĩa |
|---|---|---|
| `intervals` | `["4h", "1h", "30m", "1d"]` | Timeframes scan theo priority |
| `period` | 30 | Số candles mỗi "period" — đồng nhất mọi TF |
| `atr_period` | 14 | Chu kỳ ATR |
| `risk_pct` | 0.01 | 1% balance mỗi lệnh |
| `sl_multiplier` | 1.5 | SL = entry ± 1.5 × ATR |
| `takeProfitPercent` | 20.0 | Binance Futures ROI/PNL%; price distance = `ROI% / leverage` |
| `tp_multiplier` | 3.0 | ATR fallback khi `takeProfitPercent = 0.0` |
| `min_confidence` | 0.001 | ~0.1% move tối thiểu — gần như always-in-market |
| `scan_interval_seconds` | 900 | Rescan mỗi 15 phút |
| `max_hold_duration_seconds` | 86400 | Fallback time-exit nếu TF không có override |
| `max_hold_duration_by_interval_seconds` | 4h=432000, 1h=108000, 30m=54000, 1d=2592000 | Time-exit theo horizon 30 candles của từng TF |

---

## Điều Kiện Thị Trường Phù Hợp

Strategy hoạt động tốt nhất trong **trending market** — khi giá di chuyển một chiều liên tục qua nhiều period. Kém hiệu quả trong **choppy/sideways market** vì giá đảo chiều liên tục giữa các period, tạo nhiều tín hiệu reversal.

MTF deployment tự nhiên tạo ra sự đa dạng time horizon: `1d` bắt trend dài hạn, `30m` bắt momentum ngắn hạn.

---

## Giới Hạn & Cảnh Báo

- **Không phải true stop-and-reverse**: Engine không auto-reverse position — chỉ mở lệnh mới khi có signal đủ mạnh.
- **Period ≠ calendar month**: 30 candles trên `30m` = ~15 giờ, không phải 1 tháng. Semantic khác xa bản gốc 1933.
- **MTF priority, không phải aggregation**: `4h` có thể chặn `1h/30m` nếu mở lệnh trước cho cùng symbol.
- **Signal dùng closed candles, execution dùng latest cache close**: Direction/confidence bỏ qua forming candle; entry/TP/SL của engine có thể lấy close mới nhất làm reference.
- **Always-emit gần như không lọc**: `min_confidence = 0.001` có nghĩa là gần như mọi period close đều fire signal — cần giám sát tần suất lệnh.
- Params mặc định chưa được backtest — cần optimize `period` và `min_confidence` trước khi dùng production.
