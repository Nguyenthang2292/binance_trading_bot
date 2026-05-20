# Dow Theory

**Type:** `dow_theory`
**Version:** 1.0
**Date:** 2026-05-19
**Source ref:** `penfold-2020-universal-tactics` — Chapter 7, "Dow Theory"
**Design doc:** [docs/design/2026-05-19-strategy-dow-theory-v1.0.md](../design/2026-05-19-strategy-dow-theory-v1.0.md)

---

## Tổng Quan

Dow Theory (Charles Dow, 1900) xác định trend qua cấu trúc peak-and-trough: bull market = Higher High + Higher Low, bear market = Lower Low + Lower High. Strategy trade Long khi breakout xác nhận bull structure (HH+HL) và Short khi breakout xác nhận bear structure (LL+LH). Edge khai thác: trend persistence — một trong những hiệu ứng được kiểm chứng lâu nhất trong lịch sử kỹ thuật phân tích.

---

## Điều Kiện Vào Lệnh

| Direction | Điều kiện |
|---|---|
| **Long** | Close > last confirmed swing high **VÀ** swing high mới > swing high trước (HH) **VÀ** swing low mới > swing low trước (HL) |
| **Short** | Close < last confirmed swing low **VÀ** swing low mới < swing low trước (LL) **VÀ** swing high mới < swing high trước (LH) |

---

## Indicators Sử Dụng

| Indicator | Params | Vai trò |
|---|---|---|
| ATR | `period=14` | Swing detection threshold + engine dùng để sizing, SL và TP fallback theo `tp_multiplier` |

---

## Tham Số Mặc Định

| Param | Giá trị | Ý nghĩa |
|---|---|---|
| `intervals` | `["4h", "1h", "30m"]` | Timeframes scan |
| `atr_period` | `14` | Chu kỳ ATR (Wilder standard) |
| `swing_atr_mult` | `1.5` | Giá phải di chuyển ≥ 1.5 × ATR để confirm 1 swing point |
| `risk_pct` | `0.01` | 1% balance mỗi lệnh |
| `sl_multiplier` | `1.5` | SL = entry ± 1.5 × ATR |
| Binance Futures PNL/ROI% (`takeProfitPercent`) | `20.0` | TP mặc định theo ROI hiển thị trên Binance Futures; khoảng cách giá = ROI% / leverage |
| `tp_multiplier` | `3.0` | ATR fallback khi Binance Futures PNL/ROI% = `0.0` |
| `min_confidence` | `0.5` | Ngưỡng lọc signal yếu |
| `scan_interval_seconds` | `900` | Rescan mỗi 15 phút |
| `max_hold_duration_seconds` | `86400` | Time-exit tự động sau 24 giờ |

---

## Điều Kiện Thị Trường Phù Hợp

Hoạt động tốt nhất trong **trending market rõ ràng** — khi thị trường tạo chuỗi HH+HL hoặc LL+LH liên tục. Hiệu suất giảm đáng kể trong sideways/choppy market vì swing structure không hình thành rõ và breakout thường là false break.

---

## Giới Hạn & Cảnh Báo

- Hiệu suất kém trong ranging/sideways market — swing highs và lows không tạo cấu trúc rõ ràng
- Trễ hơn simplified Dow (single swing break) do yêu cầu cả HH+HL — bỏ lỡ một phần move đầu
- `swing_atr_mult` cần tune riêng theo asset nếu dùng ngoài default portfolio
- Params mặc định chưa được backtest — cần optimize trước khi dùng production
- Minimum 80 candles — trên timeframe thấp (30m) cần ~40 giờ lịch sử để đủ data
