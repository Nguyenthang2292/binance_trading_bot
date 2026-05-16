# Strategy: Gartley 3&6 Candle Crossover

**Version:** 1.2
**Date:** 2026-05-16
**Type:** `gartley_day_crossover`
**Status:** Design — pending implementation

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.2 | 2026-05-16 | Chốt semantic là Gartley-style crossover theo candle của từng TF; giữ invariant một symbol chỉ có một position toàn hệ thống; ATR chỉ tính tới eval candle đã đóng; `minCandles` tăng lên 16 với default ATR |
| 1.1 | 2026-05-16 | Mở rộng sang multi-timeframe: `1d`, `4h`, `1h`, `30m`; bỏ guard cứng interval; giảm scan_interval xuống 1800s |
| 1.0 | 2026-05-16 | Initial design |

---

## 1. Mục Tiêu

Gartley-style adaptation của Gartley 3&6 Week Crossover (H.M. Gartley, *Profits in the Stock Market*, 1935) sang multi-timeframe crypto futures.

Strategy dùng ba moving averages để tạo một **band động**: fast MA của mean price và hai slow MAs của candle high/low dịch chuyển về phía trước 2 candle. Khi fast MA breakout lên trên upper band → Long; xuống dưới lower band → Short; nằm trong band → None.

Các tham số `fast_period=3`, `slow_period=6`, `offset=2` luôn là **số candle trên interval đang evaluate**, không phải số ngày cố định. Ví dụ `30m` dùng 3/6/2 candle 30m; `1d` dùng 3/6/2 candle daily.

Edge khai thác: band width phản ánh volatility/regime của giai đoạn gần. Confidence hiện tại là regime score dựa trên độ rộng band; nó không đo trực tiếp breakout distance. Breakout direction vẫn do `fastMA` so với upper/lower band quyết định.

---

## 2. Signal Logic

### 2.1 Điều Kiện

| Condition | Direction | Confidence |
|---|---|---|
| `fastMA > slowHighMA[offset]` | Long | `clamp(bandWidth / mid / confThreshold, 0.0, 1.0)` |
| `fastMA < slowLowMA[offset]` | Short | `clamp(bandWidth / mid / confThreshold, 0.0, 1.0)` |
| `slowLowMA[offset] ≤ fastMA ≤ slowHighMA[offset]` | None | 0.0 |

Evaluation candle: `klines[size - 2]` — candle đã đóng gần nhất trên bất kỳ TF nào. `klines.back()` là candle đang hình thành, không được dùng cho signal hoặc ATR.

Strategy không guard theo interval cụ thể — engine chỉ gọi `evaluate()` cho các interval được liệt kê trong `config.intervals`. Thêm/bỏ TF chỉ cần sửa config, không cần đổi code.

Position invariant: một symbol chỉ có một position toàn hệ thống. Đây là trách nhiệm bắt buộc của engine/position tracker, không nằm trong strategy plugin.

### 2.2 Định Nghĩa MA

| MA | Formula | Indexing (evalIdx = size−2) |
|---|---|---|
| **fastMA** | 3-period SMA của `(high + low) / 2` | `klines[evalIdx−2 .. evalIdx]` |
| **slowHighMA[offset]** | 6-period SMA của `high`, offset 2 candle | `klines[evalIdx−7 .. evalIdx−2]` |
| **slowLowMA[offset]** | 6-period SMA của `low`, offset 2 candle | `klines[evalIdx−7 .. evalIdx−2]` |

"Offset 2 candle" có nghĩa: tại evalIdx, dùng giá trị slow MA được tính từ 2 candle trước trên cùng interval (MA kết thúc tại evalIdx − offset).

### 2.3 Confidence Formula

```
bandWidth = slowHighMA[offset] − slowLowMA[offset]
mid       = (slowHighMA[offset] + slowLowMA[offset]) / 2
confidence = clamp(bandWidth / mid / confThreshold, 0.0, 1.0)
```

`confThreshold = 0.02` (2%): band chiếm ≥ 2% giá → confidence = 1.0. Param này configurable.

### 2.4 Indicators Required

| Indicator | Params | Dùng cho |
|---|---|---|
| ATR | `period=14` | `signal.atr` — engine dùng để tính TP/SL và sizing |
| Fast MA (SMA of mean) | `fastPeriod=3` | Signal line |
| Slow High MA (SMA of high, offset) | `slowPeriod=6`, `offset=2` | Upper band |
| Slow Low MA (SMA of low, offset) | `slowPeriod=6`, `offset=2` | Lower band |

### 2.5 Minimum Data Requirements

```
minCandles = max(fastPeriod + 1, 1 + offset + slowPeriod, atrPeriod + 2)
           = max(3 + 1, 1 + 2 + 6, 14 + 2)
           = max(4, 9, 16) = 16
```

`atrPeriod + 2` là bắt buộc vì `klines.back()` có thể là candle đang hình thành; ATR phải có `atrPeriod + 1` candle đã đóng tính tới `evalIdx`. Buffer runtime hiện tại = 200 candles → đủ.

### 2.6 Assumptions

| Param | Value | Lý do |
|---|---|---|
| `fast_period` | 3 | Gartley original, diễn giải là 3 candle của TF hiện tại |
| `slow_period` | 6 | Gartley original, diễn giải là 6 candle của TF hiện tại |
| `offset` | 2 | Gartley original, diễn giải là 2 candle của TF hiện tại |
| `conf_threshold` | 0.02 | Band ≥ 2% giá → regime confidence = 1.0 |
| `atr_period` | 14 | Wilder ATR standard |
| `risk_pct` | 0.01 | 1% balance per trade |
| `sl_multiplier` | 1.5 | SL = entry ± 1.5 × ATR |
| `tp_multiplier` | 3.0 | TP = entry ± 3.0 × ATR (R:R = 1:2) |
| `min_notional` | 1.0 | Strategy floor; engine/symbol exchange filters vẫn có thể nâng floor bằng `max()` |
| `min_confidence` | 0.5 | Lọc signal với band < 1% giá |
| `scan_interval_seconds` | 1800 | Rescan mỗi 30 phút — khớp TF thấp nhất (30m) |
| `max_hold_duration_seconds` | 86400 | Time-exit sau 24 giờ |

### 2.7 Non-Goals

- Không backtest, không optimize params
- Không hedge mode
- Không tái tạo "always in market" stop-and-reverse — trả None khi trong band

---

## 3. Signal Pseudocode

```
evaluate(symbol, interval, klines):

  // Guard: không đủ data
  minCandles = max(fastPeriod + 1, 1 + offset + slowPeriod, atrPeriod + 2)
  if len(klines) < minCandles:
    return Signal(None)

  evalIdx = len(klines) - 2     // candle đã đóng gần nhất

  // Tính ATR chỉ trên candle đã đóng, không dùng klines.back()
  closedKlines = klines[0 .. evalIdx]
  atr = lastATR(closedKlines, period=atrPeriod)
  if atr <= 0:
    return Signal(None)

  // Fast MA: 3-period SMA của mean
  fastMA = avg[(klines[i].high + klines[i].low) / 2
               for i in [evalIdx-2 .. evalIdx]]

  // Slow MAs: 6-period SMA, kết thúc tại evalIdx - 2
  slowEndIdx   = evalIdx - offset              // = evalIdx - 2
  slowStartIdx = slowEndIdx - slowPeriod + 1   // = evalIdx - 7
  slowHighMA = avg[klines[i].high for i in [slowStartIdx .. slowEndIdx]]
  slowLowMA  = avg[klines[i].low  for i in [slowStartIdx .. slowEndIdx]]

  // Confidence
  bandWidth = slowHighMA - slowLowMA
  mid = (slowHighMA + slowLowMA) / 2
  if mid <= 0:
    return Signal(None)
  confidence = clamp(bandWidth / mid / confThreshold, 0.0, 1.0)

  // Long: fast MA breakout lên trên upper band
  if fastMA > slowHighMA:
    return Signal(Long, confidence, atr,
                  reason="fastMA=" + fastMA + " > slowHighMA=" + slowHighMA)

  // Short: fast MA breakout xuống dưới lower band
  if fastMA < slowLowMA:
    return Signal(Short, confidence, atr,
                  reason="fastMA=" + fastMA + " < slowLowMA=" + slowLowMA)

  return Signal(None)    // inside band
```

---

## 4. Config

```json
{
  "name": "Gartley 3&6 Candle Crossover",
  "type": "gartley_day_crossover",
  "intervals": ["1d", "4h", "1h", "30m"],
  "scan_interval_seconds": 1800,
  "max_hold_duration_seconds": 86400,
  "risk_pct": 0.01,
  "sl_multiplier": 1.5,
  "tp_multiplier": 3.0,
  "min_notional": 1.0,
  "atr_period": 14,
  "min_confidence": 0.5,
  "params": {
    "fast_period": 3,
    "slow_period": 6,
    "offset": 2,
    "conf_threshold": 0.02
  }
}
```

---

## 5. Edge Cases

| Tình huống | Xử lý |
|---|---|
| `klines.size() < minCandles` | Return `Signal{}` ngay |
| `atr <= 0` trên closed candles | Return `Signal{}` — engine skip |
| `mid <= 0` | Return `Signal{}` — giá không hợp lệ |
| `bandWidth == 0` | `confidence = 0` → bị lọc bởi `min_confidence = 0.5` |
| `fastMA == slowHighMA` hoặc `== slowLowMA` | Không trigger (điều kiện strict `>` / `<`) |
| `conf_threshold <= 0` | Parser fallback về default `0.02`; user không nên set <= 0 |

---

## 6. Implementation Notes

- ATR: dùng `strategy::indicators::lastAtr()` từ `src/strategy/indicators/atr.h` trên slice closed candles `klines[0..evalIdx]`
- Evaluation candle: `klines[size - 2]` — không dùng `klines.back()` đang hình thành cho signal hoặc ATR
- `evaluate()` phải là `const` — không lưu state, tính lại mọi thứ từ `klines` mỗi lần gọi
- `signal.atr` phải được điền khi có signal — engine dùng để sizing và TP/SL
- Không có interval guard trong `evaluate()` — engine chỉ gọi evaluate() cho các interval trong `cfg.intervals`, nên guard là thừa với multi-TF design

---

## 7. Test Plan

- Plugin config/defaults: `createStrategy()` load được, `config().intervals == ["1d", "4h", "1h", "30m"]`, `min_notional = 1.0`.
- Signal correctness: Long, Short, None, equality boundary không trigger.
- Data guard: 15 candles với default `atr_period=14` không đủ vì ATR chỉ dùng closed candles; 16 candles mới đủ.
- ATR guard: candle cuối đang hình thành với range cực lớn không được làm thay đổi `signal.atr`.
- Multi-TF: cùng logic hoạt động trên `30m`, `1h`, `4h`, `1d`; period luôn là số candle của TF được truyền vào.
- Engine integration: khi symbol đã có tracked position, signal mới của bất kỳ strategy/TF nào đều bị skip.

---

## 8. Decision Log

| Quyết định | Alternatives | Lý do chọn | Trạng thái |
|---|---|---|---|
| Diễn giải 3/6/2 là candle của từng TF | Chỉ `1d`, hoặc scale period về số ngày tương đương trên intraday | User chốt đây là Gartley-style crossover chạy theo candle từng TF | Approved |
| None khi fastMA trong band | Stop-and-reverse, midline crossover | Không vi phạm `evaluate() const`; an toàn hơn | Approved |
| Một symbol chỉ có một position toàn hệ thống | Per-strategy/per-TF positions | Đây là invariant bắt buộc của engine/position tracker | Approved |
| Confidence = band width / mid / threshold | Fixed 0.7, breakout distance | Phản ánh market regime; không phải breakout distance score | Approved |
| `conf_threshold = 0.02` | 0.01, 0.05 | 2% band là mức volatility mặc định ban đầu, cần backtest/tune | Approved |
| `klines[size-2]` làm eval candle | `klines.back()` | Tránh candle chưa đóng; ATR cũng phải dùng closed candles | Approved |
| Multi-TF: `1d`, `4h`, `1h`, `30m` | Single `1d` | Mở rộng cơ hội signal; logic MA không phụ thuộc TF cụ thể | Approved |
| Bỏ interval guard trong `evaluate()` | Guard cứng như trend_breakout | Engine đã filter theo `cfg.intervals`; guard thừa với multi-TF | Approved |
| `scan_interval` = 1800s | 3600s | Khớp TF thấp nhất 30m để không bỏ lỡ signal | Approved |
