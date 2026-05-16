# Gartley 3&6 Candle Crossover

**Type:** `gartley_day_crossover`
**Version:** 1.1
**Date:** 2026-05-16
**Design doc:** [docs/design/strategies/2026-05-16-strategy-gartley_day_crossover-v1.0.md](../design/strategies/2026-05-16-strategy-gartley_day_crossover-v1.0.md)

---

## Tổng Quan

Gartley-style adaptation của Gartley 3&6 Week Crossover (H.M. Gartley, 1935) cho crypto futures. Strategy dùng ba moving averages để tạo một band động — fast MA của mean price và hai slow MAs của candle high/low dịch chuyển về phía trước 2 candle. Signal khi fast MA breakout khỏi band; None khi nằm trong band.

Các tham số `3/6/2` là số candle trên timeframe đang evaluate, không phải số ngày cố định. Ví dụ `30m` dùng 3/6/2 candle 30m; `1d` dùng 3/6/2 candle daily.

---

## Điều Kiện Vào Lệnh

| Direction | Điều kiện |
|---|---|
| **Long** | Fast MA (SMA 3 candle của mean) vượt lên trên Upper Band (SMA 6 candle của high, lấy giá trị offset 2 candle) |
| **Short** | Fast MA xuống dưới Lower Band (SMA 6 candle của low, lấy giá trị offset 2 candle) |
| **None** | Fast MA nằm trong band — không mở lệnh |

---

## Indicators Sử Dụng

| Indicator | Params | Vai trò |
|---|---|---|
| ATR | `period=14` | Đo volatility trên candle đã đóng — engine dùng để tính TP/SL và sizing |
| Fast MA | SMA(3) của `(high + low) / 2` | Signal line |
| Slow High MA | SMA(6) của `high`, offset 2 candle | Upper band boundary |
| Slow Low MA | SMA(6) của `low`, offset 2 candle | Lower band boundary |

---

## Tham Số Mặc Định

| Param | Giá trị | Ý nghĩa |
|---|---|---|
| `intervals` | `["1d", "4h", "1h", "30m"]` | Multi-timeframe |
| `fast_period` | 3 | Chu kỳ fast MA |
| `slow_period` | 6 | Chu kỳ slow MA |
| `offset` | 2 | Số candle dịch chuyển slow MA |
| `conf_threshold` | 0.02 | Band ≥ 2% giá → regime confidence = 1.0 |
| `atr_period` | 14 | Chu kỳ ATR |
| `risk_pct` | 0.01 | 1% balance mỗi lệnh |
| `sl_multiplier` | 1.5 | SL = entry ± 1.5 × ATR |
| `tp_multiplier` | 3.0 | TP = entry ± 3.0 × ATR |
| `min_notional` | 1.0 | Strategy floor; engine/symbol filters có thể nâng floor |
| `min_confidence` | 0.5 | Lọc signal với band < 1% giá |
| `scan_interval_seconds` | 1800 | Rescan mỗi 30 phút |
| `max_hold_duration_seconds` | 86400 | Time-exit sau 24 giờ |

---

## Điều Kiện Thị Trường Phù Hợp

Hoạt động tốt nhất trong thị trường có trend/momentum rõ trên timeframe đang chạy. Kém hiệu quả trong sideways/consolidation vì signal breakout quanh band dễ nhiễu; confidence hiện tại chỉ là regime score theo band width, không đo trực tiếp breakout distance.

---

## Giới Hạn & Cảnh Báo

- Đây là Gartley-style candle crossover, không phải bản sao nguyên nghĩa 3/6 tuần hay 3/6 ngày trên intraday timeframe.
- Params mặc định chưa được backtest — cần optimize `conf_threshold`, `sl_multiplier`, `tp_multiplier` trước khi dùng production
- `conf_threshold` cần tune theo từng market; crypto volatile có thể cần giảm xuống `0.01`
- Không có trailing stop tích hợp — phụ thuộc vào `max_hold_duration_seconds` làm time-exit
- Engine giữ invariant một symbol chỉ có một position toàn hệ thống; strategy không tự mở nhiều position theo từng TF.
