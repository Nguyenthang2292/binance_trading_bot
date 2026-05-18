# Strategy: Donchian 5 and 20-Day Crossover (Crypto MTF State Variant)

**Version:** 1.0
**Date:** 2026-05-16
**Type:** `donchian_5_20_crossover`
**Status:** Design — reviewed, implementation-ready

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-05-16 | Initial design |
| 1.0-r1 | 2026-05-16 | Architecture review hardening: explicit variant identity, closed-candle evaluation, runtime param validation, and backtest caveats |

---

## 1. Mục Tiêu

Strategy dựa trên hệ thống của Richard Donchian (1960) — một trong những chiến lược trend-following lâu đời và được kiểm chứng nhất. Edge khai thác: **relative momentum** giữa short-term và long-term Simple Moving Average. Khi SMA(5) vượt trên SMA(20), thị trường đang trong uptrend và strategy giữ Long; khi SMA(5) xuyên xuống dưới SMA(20), thị trường đang trong downtrend và strategy giữ Short.

Tên strategy giữ tên gốc để truy vết nguồn Donchian, nhưng hậu tố **Crypto MTF State Variant** là bắt buộc trong `name` để tránh nhầm với nguyên bản. Nguyên bản là "always in the market" (stop-and-reverse trên dữ liệu Daily, entry next-day open). Trong context crypto bot, biến thể này emit Long hoặc Short signal dựa trên trạng thái MA hiện tại của **candle đã đóng gần nhất** mỗi lần `evaluate()` được gọi — engine chịu trách nhiệm dedup vị thế và quản lý TP/SL.

Thiết kế này không được coi là tương đương hiệu suất với Donchian nguyên bản. Việc thêm MTF, ATR bracket exit và time-exit tạo ra một strategy biến thể, cần backtest riêng trước production.

---

## 2. Understanding Lock

### 2.1 Signal Logic

| Condition | Direction | Confidence |
|---|---|---|
| SMA(short_period) > SMA(long_period) trên closed candles | Long | 1.0 |
| SMA(short_period) < SMA(long_period) trên closed candles | Short | 1.0 |
| SMA(short_period) == SMA(long_period) trên closed candles | None | 0.0 |

**Note:** Confidence cố định 1.0 là đặc trưng của biến thể binary-state này — signal chỉ có aligned hoặc không. `min_confidence = 0.5` filter sẽ luôn pass với tín hiệu Long/Short. Nếu backtest cho thấy nhiễu quanh MA cross quá cao, thêm `min_ma_spread_pct` hoặc trend-regime filter trong version sau thay vì giả lập confidence. Xem Decision Log D-1 và D-6.

### 2.2 Indicators Required

| Indicator | Params | Dùng cho |
|---|---|---|
| ATR | period=14 | `signal.atr` — engine dùng cho sizing, SL và TP fallback theo `tp_multiplier` |
| SMA | period=5 (short_period) | Short-term trend |
| SMA | period=20 (long_period) | Long-term trend |

SMA được implement local trong plugin (helper đơn giản, không dependency ngoài).

### 2.3 Minimum Data Requirements

- Strategy chỉ dùng candles có `Kline::isClosed == true`
- Cần tối thiểu **max(long_period, atr_period + 1)** closed candles
- Với default `long_period=20`, `atr_period=14`: cần tối thiểu **20 closed candles**
- Nếu cache chứa candle đang hình thành ở cuối buffer, implementation phải bỏ qua candle đó trước khi tính ATR/SMA
- Buffer hiện tại = 200 candles → **đủ** cho default params trên tất cả intervals

### 2.4 Assumptions

| Param | Value | Lý do |
|---|---|---|
| `atr_period` | 14 | Wilder ATR standard |
| `risk_pct` | 0.01 | 1% balance per trade |
| `sl_multiplier` | 1.5 | SL = entry ± 1.5 × ATR |
| `takeProfitPercent` | 20.0 | TP mặc định theo Binance Futures ROI/PNL%; khoảng cách giá = ROI% / leverage |
| `tp_multiplier` | 3.0 | ATR fallback khi `takeProfitPercent = 0.0` |
| `min_notional` | 1.0 | Khớp default hiện tại của engine/SDK |
| `min_confidence` | 0.5 | Filter safety net — confidence luôn 1.0 nên filter không active |
| `scan_interval` | 900s | Scan mỗi 15 phút — 2× per 30m candle, ≤ 900 per convention |
| `max_hold_duration` | 86400s | 24h time-exit |
| `short_period` | 5 | Như nguyên bản Donchian |
| `long_period` | 20 | Như nguyên bản Donchian |

### 2.5 Param Validation

`createStrategy()` phải reject config không hợp lệ bằng cách return `nullptr`; `evaluate()` vẫn guard defensively và return `Signal(None)` nếu gặp state không hợp lệ.

| Param | Rule |
|---|---|
| `short_period` | `> 0` |
| `long_period` | `> short_period` |
| `atr_period` | `> 0` |
| `risk_pct` | `> 0` |
| `sl_multiplier` | `> 0` |
| `tp_multiplier` | `> 0` |
| `takeProfitPercent` | `>= 0`; `0.0` uses ATR fallback |
| `min_notional` | `>= 0` |
| `min_confidence` | `0 <= min_confidence <= 1` |

### 2.6 Non-Goals

- Không implement crossover-event detection (chỉ đọc current state của MA relationship)
- Không tối ưu params (cần backtest riêng)
- Không hỗ trợ hedge mode
- Không đảm bảo "always in market" theo đúng nghĩa nguyên bản — engine có thể exit vị thế sớm qua TP/SL trước khi có crossover ngược chiều
- Không claim kết quả lịch sử của Donchian nguyên bản cho biến thể crypto/MTF này

---

## 3. Signal Pseudocode

```
evaluate(symbol, interval, klines):

  closed = filter(klines, kline.isClosed)

  // Guard: config hoặc data không hợp lệ
  if short_period <= 0 or long_period <= short_period or atr_period <= 0:
    return Signal(None)

  min_candles = max(long_period, atr_period + 1)
  if len(closed) < min_candles:
    return Signal(None)

  // Tính ATR — bắt buộc cho engine
  atr = lastATR(closed, period=atr_period)
  if atr <= 0:
    return Signal(None)

  // Tính SMA trên candle đã đóng gần nhất
  smaShort = mean(closed[-short_period:].close)
  smaLong  = mean(closed[-long_period:].close)

  // Long condition
  if smaShort > smaLong:
    return Signal(Long, confidence=1.0, atr=atr,
                  reason="SMA5=XX.XX > SMA20=XX.XX")

  // Short condition
  if smaShort < smaLong:
    return Signal(Short, confidence=1.0, atr=atr,
                  reason="SMA5=XX.XX < SMA20=XX.XX")

  // Guard: smaShort == smaLong (near impossible with real price data)
  return Signal(None)
```

---

## 4. Config

```json
{
  "name": "Donchian 5 and 20-Day Crossover (Crypto MTF State Variant)",
  "type": "donchian_5_20_crossover",
  "intervals": ["1d", "4h", "1h", "30m"],
  "scan_interval_seconds": 900,
  "max_hold_duration_seconds": 86400,
  "risk_pct": 0.01,
  "sl_multiplier": 1.5,
  "tp_multiplier": 3.0,
  "takeProfitPercent": 20.0,
  "min_notional": 1.0,
  "atr_period": 14,
  "min_confidence": 0.5,
  "params": {
    "short_period": 5,
    "long_period": 20
  }
}
```

---

## 5. Edge Cases

| Tình huống | Xử lý |
|---|---|
| `closed_klines.size() < max(long_period, atr_period + 1)` | Return `Signal(None)` ngay |
| `atr == 0` | Return `Signal(None)` — engine sẽ skip |
| `sma5 == sma20` | Return `Signal(None)` — guard case, gần như không xảy ra với dữ liệu thực |
| `short_period <= 0` hoặc `long_period <= short_period` | `createStrategy()` reject config; `evaluate()` return `Signal(None)` nếu vẫn gặp state này |
| `atr_period <= 0` | `createStrategy()` reject config; `evaluate()` return `Signal(None)` nếu vẫn gặp state này |
| Daily candle chưa đóng hoặc candle hiện tại đang mở | Bỏ qua, chỉ tính SMA/ATR trên closed candles để tránh repaint |

---

## 6. Implementation Notes

> Dành cho developer implement `evaluate()`:

- ATR đã có sẵn tại `src/strategy/indicators/atr.h` — dùng `strategy::indicators::lastAtr()`
- SMA là local helper trong plugin — implement như loop sum / period trên last `period` closes
- Trước khi tính ATR/SMA, build `closedKlines` bằng cách lấy các candles `isClosed == true`; không dùng candle đang mở
- `createStrategy()` validate params và config risk fields; invalid config return `nullptr`
- `evaluate()` tính `minCandles = max(long_period, atr_period + 1)` thay vì hard-code 25
- `evaluate()` phải là `const` và **không được giữ state** — tính lại mọi thứ từ `klines` mỗi lần gọi
- `signal.atr` PHẢI được điền — engine dùng để sizing, SL và TP fallback theo `tp_multiplier`
- **Always-signaling behavior**: strategy trả Long hoặc Short gần như mọi lúc. Engine phải có logic dedup để không mở duplicate position theo cùng direction. Điều này nằm ngoài scope của strategy plugin.
- **Reason string format**: dùng `std::fixed << std::setprecision(2)` để giữ 2 decimal (reviewer U-1)
- `"1d"` interval: signal fire mỗi 900s nhưng daily MA chỉ thay đổi khi có daily candle mới đóng. Behavior này là expected — engine dedup xử lý.
- Tham khảo `docs/sdk/writing-a-strategy-plugin.md` cho DLL setup chi tiết

---

## 7. Decision Log

| Quyết định | Alternatives | Lý do chọn | Trạng thái |
|---|---|---|---|
| D-1: Confidence = 1.0 cố định | Dùng normalized distance `\|sma5-sma20\|/sma20` | Biến thể này là binary state signal. Không dùng confidence để che nhiễu; nếu cần lọc nhiễu thì thêm filter explicit và backtest riêng. | Approved |
| D-2: SMA thay vì EMA | EMA (phản ứng nhanh hơn) | Nguyên bản Donchian 1960 dùng Simple Moving Average. Thay đổi sang EMA là khác strategy. | Approved |
| D-3: Intervals `["1d", "4h", "1h", "30m"]` | Chỉ `["1d"]` hoặc chỉ `["4h", "1h", "30m"]` | Giữ `"1d"` để bảo tồn anchor của strategy gốc, thêm MTF để phù hợp crypto bot. Hậu tố name bắt buộc đánh dấu đây là variant, không phải nguyên bản thuần Daily. | Approved |
| D-4: Stop-and-reverse → signal-based + ATR exits | Implement stop-and-reverse thực sự | Engine không hỗ trợ stop-and-reverse native. Strategy emit signal dựa trên current closed-candle MA state — engine dedup và ATR TP/SL xử lý phần còn lại. Đây là behavior biến thể và phải backtest riêng. | Approved |
| D-5: `short_period` và `long_period` expose qua `params{}` | Hard-code 5 và 20 | Cho phép user thử các combination khác (10/50, 5/10) mà không cần recompile. Không vi phạm YAGNI vì struct đã có. | Approved |
| D-6: Chỉ dùng closed candles | Dùng candle đang mở để phản ứng nhanh hơn | MA crossover trên candle đang mở có repaint risk cao, đặc biệt với Daily. Closed-candle evaluation gần với Donchian gốc hơn và ổn định hơn cho backtest/live parity. | Approved |
| D-7: Validate params tại plugin boundary | Undefined behavior, user tự chịu trách nhiệm | Params là runtime config; invalid config không được dẫn tới out-of-bounds hoặc signal sai. Reject sớm giúp vận hành an toàn hơn. | Approved |
