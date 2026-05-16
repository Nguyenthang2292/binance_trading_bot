# Donchian 5 and 20-Day Crossover (Crypto MTF State Variant)

**Type:** `donchian_5_20_crossover`
**Version:** 1.0
**Date:** 2026-05-16
**Design doc:** [docs/design/2026-05-16-strategy-donchian_5_20_crossover-v1.0.md](../design/2026-05-16-strategy-donchian_5_20_crossover-v1.0.md)

---

## Tổng Quan

Biến thể crypto của hệ thống Richard Donchian (1960) — một trong những chiến lược trend-following lâu đời nhất còn được kiểm chứng. So sánh SMA(5) và SMA(20) trên **closed candles** để xác định hướng trend hiện tại và emit Long/Short signal tương ứng. Hậu tố "Crypto MTF State Variant" phân biệt với nguyên bản Daily stop-and-reverse: biến thể này chạy trên 4 timeframes đồng thời và dùng ATR bracket exits thay vì chỉ exit khi crossover ngược chiều — cần backtest riêng, không kế thừa kết quả lịch sử của Donchian nguyên bản.

---

## Điều Kiện Vào Lệnh

| Direction | Điều kiện |
|---|---|
| **Long** | SMA(5) > SMA(20) trên closed candles — short-term trend mạnh hơn long-term |
| **Short** | SMA(5) < SMA(20) trên closed candles — short-term trend yếu hơn long-term |

---

## Indicators Sử Dụng

| Indicator | Params | Vai trò |
|---|---|---|
| ATR | period=14 | Đo volatility — engine dùng để tính TP/SL và sizing |
| SMA | period=5 (short_period) | Short-term trend momentum, tính trên closed candles |
| SMA | period=20 (long_period) | Long-term trend baseline, tính trên closed candles |

---

## Tham Số Mặc Định

| Param | Giá trị | Ý nghĩa |
|---|---|---|
| `intervals` | `["1d", "4h", "1h", "30m"]` | `"1d"` honor strategy gốc; 3 còn lại là MTF must-have |
| `atr_period` | 14 | Chu kỳ ATR |
| `risk_pct` | 0.01 | 1% balance mỗi lệnh |
| `sl_multiplier` | 1.5 | SL = entry ± 1.5 × ATR |
| `tp_multiplier` | 3.0 | TP = entry ± 3.0 × ATR |
| `min_confidence` | 0.5 | Safety net — confidence luôn 1.0 nên filter không active |
| `scan_interval_seconds` | 1800 | Rescan mỗi 30 phút |
| `max_hold_duration_seconds` | 86400 | Time-exit tự động sau 24h |
| `short_period` | 5 | Override via `params.short_period` (phải < `long_period`) |
| `long_period` | 20 | Override via `params.long_period` (phải > `short_period`) |

---

## Điều Kiện Thị Trường Phù Hợp

Hoạt động tốt nhất trong **trending market** — khi giá có xu hướng rõ ràng và duy trì trong thời gian dài. Hiệu suất kém trong sideways/choppy market do crossover xảy ra thường xuyên mà không có follow-through.

---

## Giới Hạn & Cảnh Báo

- Đây là **biến thể** — không kế thừa kết quả lịch sử của Donchian nguyên bản (Daily, stop-and-reverse)
- Hiệu suất kém trong ranging / choppy market — nhiều false crossover
- Strategy "always signaling" — gần như luôn emit Long hoặc Short; engine cần dedup logic để tránh duplicate position
- ATR bracket exits có thể đóng vị thế sớm hơn nguyên bản (vốn chỉ exit khi có crossover ngược chiều)
- Params mặc định chưa được backtest — cần optimize trước khi dùng production
