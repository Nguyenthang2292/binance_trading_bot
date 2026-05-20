# Strategy: Dow Theory

**Version:** 1.1
**Date:** 2026-05-19
**Type:** `dow_theory`
**Status:** Design - ready for implementation

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.1 | 2026-05-19 | Review fixes: closed-candle contract, ordered pivot model, breakout crossing dedupe, ATR series swing threshold, implementation checklist and test acceptance criteria |
| 1.0 | 2026-05-19 | Initial design |

---

## 1. Mục Tiêu

Dow Theory (Charles Dow, 1900) xác nhận trend qua cấu trúc peak-and-trough. Strategy trade Long khi thị trường tạo Higher High + Higher Low (bull structure) và candle đã đóng breakout lên trên confirmed swing high. Strategy trade Short khi thị trường tạo Lower Low + Lower High (bear structure) và candle đã đóng breakout xuống dưới confirmed swing low.

Edge mà strategy khai thác là **trend persistence**: sau khi cấu trúc trend đã hình thành và giá đóng cửa breakout qua pivot xác nhận, xác suất trend tiếp tục có thể cao hơn xác suất đảo chiều ngay lập tức.

Lưu ý scope: thiết kế này là biến thể runtime cho Binance Futures scanner hiện tại, không phải bản mechanical Dow Theory gốc. Bản gốc trong nguồn tham khảo dùng daily data, always-in-market, stop-and-reverse, không dùng ATR, fixed SL/TP hay time exit. Vì vậy mọi claim production phải được xác nhận bằng backtest/shadow run riêng cho biến thể này.

---

## 2. Understanding Lock

### 2.1 Runtime Contract

Strategy **chỉ được đánh giá trên closed candles**.

- Input `klines` có thể chứa candle đang chạy từ WebSocket cache.
- `evaluate()` phải tạo `closed = klines where kline.isClosed == true`.
- Nếu dữ liệu REST warmup đánh dấu tất cả candles là closed, toàn bộ candles đó được dùng.
- Breakout candle là `closed[-1]`.
- Previous close dùng cho dedupe là `closed[-2].close`.
- Candle đang chạy không được dùng cho ATR, swing detection, breakout check, hoặc previous close.

Lý do: engine scan theo timer, không chỉ theo sự kiện close. Nếu dùng `klines.back()` trực tiếp, strategy có thể vào lệnh theo intrabar wick, trái với quyết định "candle close confirmation".

### 2.2 Pivot Model

Swing detection phải trả về ordered pivots, không chỉ 2 list giá riêng biệt.

```cpp
enum class PivotType { High, Low };

struct Pivot {
    PivotType type;
    double price;
    std::size_t index;       // index trong closed candles
    int64_t openTime;
    std::size_t confirmedAt; // index candle xác nhận đảo chiều
    double atrAtConfirm;
};
```

Required invariants:

- `pivots` sắp xếp tăng dần theo `index`.
- Pivots phải alternating theo type sau normalization: `High, Low, High, Low` hoặc `Low, High, Low, High`.
- Nếu phát hiện pivot cùng type liên tiếp trước khi có pivot đối diện, giữ extreme mạnh hơn:
  - với `High`, giữ giá cao hơn;
  - với `Low`, giữ giá thấp hơn.
- Candle cuối trong `closed` có thể dùng để confirm pivot trước đó, nhưng pivot tại chính candle cuối không được coi là confirmed nếu chưa có reversal sau nó.

### 2.3 Signal Logic

Sau khi có ordered pivots, lấy 2 swing highs gần nhất và 2 swing lows gần nhất nhưng vẫn phải kiểm tra chronology bằng `index`.

| Condition | Direction | Confidence |
|---|---|---|
| `sh2.price > sh1.price` và `sl2.price > sl1.price` và `sl2.index > sh1.index` và `sh2.index > sl1.index` và `prevClose <= sh2.price` và `close > sh2.price` | Long | `(breakout_strength + structure_quality) / 2` |
| `sl2.price < sl1.price` và `sh2.price < sh1.price` và `sh2.index > sl1.index` và `sl2.index > sh1.index` và `prevClose >= sl2.price` và `close < sl2.price` | Short | `(breakout_strength + structure_quality) / 2` |
| Không thỏa điều kiện nào | None | 0.0 |

`prevClose <= sh2.price && close > sh2.price` và `prevClose >= sl2.price && close < sl2.price` là crossing rule để strategy stateless nhưng chỉ signal khi có breakout mới. Không được emit lại cùng breakout ở các scan cycle sau khi giá vẫn nằm ngoài swing level.

**Confidence components:**

```
breakout_strength = clamp(abs(close - swing_point) / atr, 0.0, 1.0)
structure_quality = clamp(avg(abs(SH2 - SH1), abs(SL2 - SL1)) / atr, 0.0, 1.0)
confidence        = (breakout_strength + structure_quality) / 2
```

Trong đó `atr` là ATR của breakout candle (`closed[-1]`), dùng cho `signal.atr` và confidence.

### 2.4 Indicators Required

| Indicator | Params | Dùng cho |
|---|---|---|
| ATR series | `period=14` | Swing confirmation threshold tại từng candle và `signal.atr` cho engine sizing, SL, TP fallback |

Implementation phải dùng `strategy::indicators::atr(closed, atr_period)` để lấy ATR series. Không dùng một ATR cuối cho toàn bộ lịch sử swing scan.

### 2.5 Minimum Data Requirements

- Cần tối thiểu **80 closed candles** để scan 2 confirmed swing highs + 2 confirmed swing lows với ATR-based detection.
- Runtime cache hiện tại có buffer khoảng 200 candles, đủ nếu scanner warmup/backfill thành công.
- `main.cpp::minWarmupCandles()` phải thêm branch `dow_theory` trả ít nhất `80` để warning coverage đúng.

### 2.6 Assumptions

| Param | Value | Lý do |
|---|---|---|
| `atr_period` | 14 | Wilder ATR standard |
| `swing_atr_mult` | 1.5 | Giá phải đảo chiều ít nhất 1.5 ATR tại candle confirm để xác nhận swing |
| `risk_pct` | 0.01 | 1% balance per trade |
| `sl_multiplier` | 1.5 | SL engine fallback = entry +/- 1.5 ATR |
| Binance Futures PNL/ROI% (`takeProfitPercent`) | 20.0 | TP theo ROI hiển thị trên Binance Futures; khoảng cách giá = ROI% / leverage |
| `tp_multiplier` | 3.0 | ATR fallback khi Binance Futures PNL/ROI% = `0.0` |
| `min_notional` | 1.0 | Khớp default hiện tại của engine/SDK |
| `min_confidence` | 0.5 | Filter signal yếu |
| `scan_interval` | 900s | Rescan mỗi 15 phút |
| `max_hold_duration` | 86400s | Time-exit sau 24 giờ |
| `MIN_CLOSED_CANDLES` | 80 | Đủ để scan 2 confirmed swings mỗi loại |

### 2.7 Non-Goals

- Không tối ưu params trong design này.
- Không hỗ trợ hedge mode.
- Không implement always-in-market stop-and-reverse.
- Không dùng volume hoặc indicator phụ.
- Không thêm persistent state/dedupe store trong strategy v1.1; crossing rule xử lý dedupe ở mức stateless.

---

## 3. Signal Pseudocode

```
evaluate(symbol, interval, klines):

  closed = [k for k in klines if k.isClosed]

  if len(closed) < 80:
    return Signal(None)

  atr_values = ATR(closed, period=14)
  atr = atr_values[-1]
  if atr <= 0:
    return Signal(None)

  pivots = detectPivots(closed, atr_values, swing_atr_mult=1.5)

  highs = [p for p in pivots if p.type == High]
  lows  = [p for p in pivots if p.type == Low]

  if len(highs) < 2 or len(lows) < 2:
    return Signal(None)

  sh1 = highs[-2]
  sh2 = highs[-1]
  sl1 = lows[-2]
  sl2 = lows[-1]

  prevClose = closed[-2].close
  close     = closed[-1].close

  bullStructure =
    sh2.price > sh1.price and
    sl2.price > sl1.price and
    sl2.index > sh1.index and
    sh2.index > sl1.index

  bearStructure =
    sl2.price < sl1.price and
    sh2.price < sh1.price and
    sh2.index > sl1.index and
    sl2.index > sh1.index

  if bullStructure and prevClose <= sh2.price and close > sh2.price:
    breakout_strength = clamp((close - sh2.price) / atr, 0.0, 1.0)
    structure_quality = clamp(((sh2.price - sh1.price) + (sl2.price - sl1.price)) / 2 / atr, 0.0, 1.0)
    confidence = (breakout_strength + structure_quality) / 2
    return Signal(Long, confidence, atr,
                  reason="tf=" + interval + " Dow bull: SH1=" + sh1.price
                         + " SH2=" + sh2.price + " SL1=" + sl1.price
                         + " SL2=" + sl2.price + " close=" + close)

  if bearStructure and prevClose >= sl2.price and close < sl2.price:
    breakout_strength = clamp((sl2.price - close) / atr, 0.0, 1.0)
    structure_quality = clamp(((sl1.price - sl2.price) + (sh1.price - sh2.price)) / 2 / atr, 0.0, 1.0)
    confidence = (breakout_strength + structure_quality) / 2
    return Signal(Short, confidence, atr,
                  reason="tf=" + interval + " Dow bear: SL1=" + sl1.price
                         + " SL2=" + sl2.price + " SH1=" + sh1.price
                         + " SH2=" + sh2.price + " close=" + close)

  return Signal(None)


detectPivots(closed, atr_values, swing_atr_mult):

  pivots = []
  if len(closed) < 2:
    return pivots

  direction = Unknown
  extremeHigh = closed[0].high
  extremeHighIndex = 0
  extremeLow = closed[0].low
  extremeLowIndex = 0

  for i from 1 to len(closed) - 1:
    atr_i = atr_values[i]
    if atr_i <= 0:
      continue

    threshold = swing_atr_mult * atr_i
    k = closed[i]

    if k.high > extremeHigh:
      extremeHigh = k.high
      extremeHighIndex = i

    if k.low < extremeLow:
      extremeLow = k.low
      extremeLowIndex = i

    if direction != Down and extremeHigh - k.low >= threshold and extremeHighIndex < i:
      addOrReplacePivot(High, extremeHigh, extremeHighIndex, confirmedAt=i, atrAtConfirm=atr_i)
      direction = Down
      extremeLow = k.low
      extremeLowIndex = i
      continue

    if direction != Up and k.high - extremeLow >= threshold and extremeLowIndex < i:
      addOrReplacePivot(Low, extremeLow, extremeLowIndex, confirmedAt=i, atrAtConfirm=atr_i)
      direction = Up
      extremeHigh = k.high
      extremeHighIndex = i
      continue

  return normalized alternating pivots
```

`addOrReplacePivot()` must preserve the alternating invariant. If the new pivot has the same type as the last pivot, replace only when it is the stronger extreme.

---

## 4. Config

```json
{
  "name": "Dow Theory",
  "type": "dow_theory",
  "intervals": ["4h", "1h", "30m"],
  "scan_interval_seconds": 900,
  "max_hold_duration_seconds": 86400,
  "risk_pct": 0.01,
  "sl_multiplier": 1.5,
  "takeProfitPercent": 20.0,
  "tp_multiplier": 3.0,
  "min_notional": 1.0,
  "atr_period": 14,
  "min_confidence": 0.5,
  "params": {
    "swing_atr_mult": 1.5
  }
}
```

Config validation:

- `atr_period > 0`, otherwise `createStrategy()` returns `nullptr` or normalizes to 14 consistently with existing plugin conventions.
- `swing_atr_mult >= 0.5`; recommended minimum is 1.0. Values below 0.5 should be rejected to avoid noisy pivot churn.
- `tp_multiplier > 0` when `takeProfitPercent == 0.0`.
- `min_confidence` must be within `[0.0, 1.0]`.

---

## 5. Edge Cases

| Tình huống | Xử lý |
|---|---|
| Fewer than 80 closed candles | Return `Signal(None)` |
| `closed.size() < 2` after filtering forming candles | Return `Signal(None)` |
| ATR series last value `<= 0` | Return `Signal(None)` |
| ATR value at an intermediate candle `<= 0` | Skip that candle for swing confirmation |
| Ít hơn 2 confirmed swing highs hoặc lows | Return `Signal(None)` |
| Pivots cùng type liên tiếp | Normalize bằng stronger extreme rule |
| Swing highs/lows bằng nhau | HH/HL hoặc LL/LH không thỏa vì dùng strict `>` và `<` |
| Close bằng đúng swing point | Không signal vì breakout dùng strict `>` và `<` |
| Previous close đã nằm ngoài swing level | Không signal lại vì crossing rule không thỏa |
| Candle đang chạy breakout nhưng chưa close | Không signal vì forming candle bị loại khỏi `closed` |
| `swing_atr_mult` quá nhỏ | Config reject nếu `< 0.5`; log/warn nếu `< 1.0` là optional |
| Scanner warmup < 80 | Startup warning qua `minWarmupCandles()`; runtime vẫn return None đến khi đủ data |

---

## 6. Implementation Notes

> Dành cho developer implement `evaluate()` và `detectPivots()`.

- Update `plugins/src/dow_theory/strategy_dow_theory.cpp`; thay TODO `detectSwings()` bằng ordered pivot implementation như Section 3.
- `evaluate()` phải là `const` và stateless.
- Filter closed candles trước khi tính ATR và pivots.
- Dùng `strategy::indicators::atr()` cho ATR series; dùng `atr_values.back()` cho `signal.atr`.
- Không dùng `klines.back()` trực tiếp trừ khi nó đã lọc qua `closed`.
- Reason string nên include interval, pivot prices, close, và có thể include pivot indexes để debug.
- `detectPivots()` là helper trong anonymous namespace, không expose qua ABI.
- Thêm `add_subdirectory(plugins/src/dow_theory)` vào top-level `CMakeLists.txt`.
- Thêm `strategy_dow_theory` vào dependencies của test executable.
- Thêm branch `dow_theory` vào `main.cpp::minWarmupCandles()` để return ít nhất `80`.
- Chỉ thêm entry `dow_theory` vào `config.json` khi muốn bật strategy ở runtime; design ready không bắt buộc bật production ngay.

---

## 7. Test Acceptance Criteria

Thêm `tests/test_dow_theory_plugin.cpp` với các case tối thiểu:

| Test | Expected |
|---|---|
| Loads plugin and default config | `strategyType() == "dow_theory"`, intervals `["4h", "1h", "30m"]`, `takeProfitPercent == 20.0` |
| Emits long on closed-candle HH/HL crossing | Direction Long, confidence `>= min_confidence`, `atr > 0`, reason contains `Dow bull` |
| Emits short on closed-candle LL/LH crossing | Direction Short, confidence `>= min_confidence`, `atr > 0`, reason contains `Dow bear` |
| Ignores forming candle breakout | Last non-closed candle crosses level but `closed[-1]` does not, returns None |
| Does not repeat breakout | `prevClose > sh2.price` and `close > sh2.price` returns None for long; symmetric for short |
| Strict equality | `close == swing level` returns None |
| Insufficient closed data | Fewer than 80 closed candles returns None |
| Unconfigured interval | Returns None |
| Same-type pivot normalization | Consecutive highs keep the higher high; consecutive lows keep the lower low |
| ATR series threshold | Historical high-vol and low-vol segments use their own ATR values, not final ATR only |

Recommended verification commands:

```powershell
rtk cmake --build build --config Debug --target strategy_dow_theory
rtk ctest --test-dir build -C Debug -R "dow_theory|strategy"
```

---

## 8. Rollout Plan

1. Implement plugin and tests.
2. Build and run plugin tests locally.
3. Add config entry with small symbol universe or testnet first.
4. Run shadow/log-only observation by keeping order placement disabled externally or using testnet.
5. Backtest or replay at least 30m, 1h, 4h samples before enabling with real capital.

---

## 9. Decision Log

| Quyết định | Alternatives | Lý do chọn | Trạng thái |
|---|---|---|---|
| Closed-candle-only evaluation | Use `klines.back()` directly | Engine scans by timer and cache can contain forming candle; closed-only preserves candle close confirmation | Approved |
| Ordered pivot list with type/index/openTime | Separate `highs` and `lows` vectors | Prevents mixing unrelated highs/lows and makes Dow structure chronology explicit | Approved |
| Stateless breakout crossing rule | Persistent signal dedupe store | Keeps plugin ABI simple while preventing repeated signals for the same breakout | Approved |
| ATR series for swing threshold | Single final ATR for all history | Preserves volatility adaptation across changing regimes | Approved |
| Breakout-only signal | Always in market stop-and-reverse | Matches current engine where SL/TP/time-exit manage exits and one tracked position per symbol prevents flip churn | Approved |
| Strict Dow structure HH+HL / LL+LH | Single swing break | Reduces false signals and keeps the strategy close to peak-and-trough trend logic | Approved |
| MIN_CLOSED_CANDLES = 80 | 50, 100 | Conservative minimum for 2 confirmed swings each side under ATR-based detection | Approved |
| `swing_atr_mult` default = 1.5 | 1.0, 2.0, 3.0 | Reasonable default for reversal confirmation before backtest tuning | Approved |

---

## 10. Ready Checklist

- [x] Closed-candle contract specified.
- [x] Pivot ordering and alternation specified.
- [x] Breakout dedupe specified without strategy state.
- [x] ATR series requirement specified.
- [x] Source/edge claim scoped to adapted runtime variant.
- [x] CMake, warmup, and test integration tasks listed.
- [x] Acceptance tests listed.

