# Golden 50/200 Moving Average Crossover (Crypto MTF State Variant)

**Type:** `golden_crossover`
**Version:** 1.0
**Date:** 2026-05-16
**Design doc:** [docs/design/2026-05-16-strategy-golden_crossover-v1.0.md](../design/2026-05-16-strategy-golden_crossover-v1.0.md)

---

## Tổng Quan

Biến thể crypto của chiến lược Golden Cross / Death Cross kinh điển (Brent Penfold, 2020) — một trong những tín hiệu trend-following được trích dẫn nhiều nhất trong phân tích kỹ thuật. So sánh SMA(50) và SMA(200) trên **closed candles** để xác định hướng trend hiện tại và emit Long/Short signal tương ứng. Hậu tố "Crypto MTF State Variant" phân biệt với nguyên bản Daily stop-and-reverse: biến thể này chạy trên 3 timeframes đồng thời và dùng ATR bracket exits thay vì chỉ exit khi crossover ngược chiều — cần backtest riêng, không kế thừa kết quả lịch sử của nguyên bản.

---

## Điều Kiện Vào Lệnh

| Direction | Điều kiện |
|---|---|
| **Long** | SMA(50) > SMA(200) — Golden Cross state: short-term trend mạnh hơn long-term |
| **Short** | SMA(50) < SMA(200) — Death Cross state: short-term trend yếu hơn long-term |

---

## Indicators Sử Dụng

| Indicator | Params | Vai trò |
|---|---|---|
| ATR | period=14 | Đo volatility — engine dùng để sizing, SL và TP fallback |
| SMA | period=50 (`ma_short`) | Short-term trend momentum |
| SMA | period=200 (`ma_long`) | Long-term trend baseline |

---

## Tham Số Mặc Định

| Param | Giá trị | Ý nghĩa |
|---|---|---|
| `intervals` | `["4h", "1h", "30m"]` | Timeframes scan — MTF must-have |
| `atr_period` | 14 | Chu kỳ ATR |
| `risk_pct` | 0.01 | 1% balance mỗi lệnh |
| `sl_multiplier` | 1.5 | SL = entry ± 1.5 × ATR |
| `takeProfitPercent` | 20.0 | TP mặc định theo Binance Futures ROI/PNL%; khoảng cách giá = ROI% / leverage |
| `tp_multiplier` | 3.0 | ATR fallback khi `takeProfitPercent = 0.0` |
| `min_confidence` | 0.5 | Floor của confidence — dynamic formula: `clamp(spread/0.01, 0.5, 1.0)` |
| `scan_interval_seconds` | 900 | Rescan mỗi 15 phút |
| `max_hold_duration_seconds` | 259200 | Time-exit tự động sau 3 ngày |
| `ma_short` | 50 | Override via `params.ma_short` (phải < `ma_long`) |
| `ma_long` | 200 | Override via `params.ma_long` (phải > `ma_short`) |

---

## Điều Kiện Thị Trường Phù Hợp

Hoạt động tốt nhất trong **trending market** rõ ràng và kéo dài — khi giá có xu hướng duy trì nhiều ngày đến nhiều tuần. MA(200) là slow indicator, cần momentum mạnh để tạo crossover. Hiệu suất kém trong sideways/choppy market do SMA(50) và SMA(200) liên tục đan xen, tạo nhiều false signals.

---

## Giới Hạn & Cảnh Báo

- Đây là **biến thể** — không kế thừa kết quả lịch sử của nguyên bản (Daily futures, stop-and-reverse)
- **Buffer constraint:** SMA(200) cần 200 closed candles; runtime config hiện dùng `warmup_initial_limit=200` và `kline_buffer_size=201`
- **Semantic mismatch:** Trên 30m TF, SMA(200) = ~100 giờ (không phải 200 ngày); signal quality thấp hơn rõ rệt so với 4h
- Strategy "always signaling" khi đủ data — engine cần dedup logic để tránh duplicate position
- ATR bracket exits có thể đóng vị thế trước crossover ngược chiều (khác nguyên bản)
- Không có sideways market filter — hiệu suất kém trong giai đoạn tích lũy/ranging
- Params mặc định chưa được backtest trên crypto — cần optimize trước khi dùng production
