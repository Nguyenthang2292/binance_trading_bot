# Strategy: Ricardo Rules

**Version:** 1.0
**Date:** 2026-05-19
**Type:** `ricardo_rules`
**Status:** Design — requires engine extension for exact Ricardo exits

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-05-19 | Initial design |

---

## 1. Mục Tiêu

Hiện thực hóa ba nguyên tắc giao dịch của David Ricardo (1800) dưới dạng plugin tự động:

1. **Never refuse an option** — vào lệnh ngay khi giá phá vỡ bar trước
2. **Cut your losses short** — đặt initial stop tại lowest low / highest high của setup và entry bar
3. **Let your profits run** — trailing stop theo swing point gần nhất

Strategy khai thác **momentum breakout** thuần price-action, không dùng indicator nào ngoài ATR (phục vụ sizing/confidence). Edge đến từ việc tham gia ngay khi thị trường xác nhận hướng qua price action — không filter, không delay. Để giữ đúng Ricardo Rules, engine phải hỗ trợ custom initial stop và swing-point trailing stop thật thay vì ép strategy dùng ATR SL/TP cố định.

---

## 2. Understanding Lock

### 2.1 Signal Logic

| Condition | Direction | Confidence |
|---|---|---|
| `kEval.close > setupBar.high` (close bar vừa đóng vượt high bar trước) | Long | `clamp((kEval.close − setupBar.high) / atr, 0.0, 1.0)` |
| `kEval.close < setupBar.low` (close bar vừa đóng dưới low bar trước) | Short | `clamp((setupBar.low − kEval.close) / atr, 0.0, 1.0)` |
| Không thỏa điều kiện nào | None | 0.0 |

**Định nghĩa bars:**
- `kEval` = `klines[size-2]` — bar hoàn toàn đóng gần nhất (entry bar trong Ricardo Rules)
- `setupBar` = `klines[size-3]` — bar trước đó (setup bar — high/low của bar này bị phá vỡ)
- `klines.back()` = bar đang hình thành → không dùng

### 2.2 Indicators Required

| Indicator | Params | Dùng cho |
|---|---|---|
| ATR | `period = atr_period`, tính trên `closedKlines = klines[0..size-2]` | `signal.atr` — engine dùng cho sizing và confidence; không dùng candle đang hình thành |

Không có indicator nào khác. Ricardo Rules là pure price-action.

### 2.3 Minimum Data Requirements

- Cần tối thiểu **`atr_period + 2` candles** tổng cộng: `atr_period + 1` candles đã đóng để tính ATR tại `kEval`, cộng 1 forming bar bị loại khỏi tính toán
- Với default `atr_period = 14`: MIN_CANDLES = **16**
- Buffer hiện tại = 200 candles → **đủ**

### 2.4 Assumptions

| Param | Value | Lý do |
|---|---|---|
| `atr_period` | 14 | Wilder ATR standard |
| `risk_pct` | 0.01 | 1% balance per trade |
| `sl_multiplier` | 1.5 | Legacy fallback only; exact Ricardo dùng custom initial stop tại setup/entry extreme |
| Binance Futures PNL/ROI% (`takeProfitPercent`) | 0.0 / disabled | Không đặt fixed TP để đúng "let profits run"; cần engine hỗ trợ no-fixed-TP exit policy |
| `tp_multiplier` | 0.0 / disabled | Không dùng ATR TP fallback khi bật swing trailing thật |
| `min_notional` | 1.0 | Khớp default hiện tại của engine/SDK |
| `min_confidence` | 0.0 | Giữ đúng "Never refuse an option"; confidence chỉ dùng telemetry/ranking |
| `scan_interval` | 900s | Rescan mỗi 15 phút — 2× per 30m candle |
| `max_hold_duration` | 86400s | Time-exit 24h |
| `swing_lookback` | 3 | N bars mỗi bên để xác nhận swing point cho trailing stop thật |

### 2.5 Engine Extension Requirements

Để strategy này là Ricardo Rules đúng nghĩa, engine cần mở rộng contract thay vì đọc stop từ `reason` string:

- `Signal` hoặc một execution-plan struct tương đương phải mang `initial_stop_price` do strategy trả về.
- Engine đặt initial stop đúng tại:
  - Long: 1 tick dưới `min(setupBar.low, kEval.low)`
  - Short: 1 tick trên `max(setupBar.high, kEval.high)`
- Engine hỗ trợ exit policy không có fixed take-profit; vị thế thoát bằng initial stop, swing trailing stop, hoặc time-exit.
- Engine hỗ trợ swing-point trailing stop thật:
  - Long: dời stop lên 1 tick dưới swing low gần nhất đã xác nhận
  - Short: dời stop xuống 1 tick trên swing high gần nhất đã xác nhận
  - Chỉ dùng candles đã đóng, không dùng `klines.back()` nếu candle đó đang hình thành
  - Không bao giờ dời stop theo hướng bất lợi
- `swing_lookback` phải được persist cùng tracked position để trailing monitor dùng nhất quán sau khi entry.

### 2.6 Non-Goals

- **Neutral bar (inside bar) setup filter**: Penfold mô tả setup là "neutral daily bar", nhưng mô tả tổng quát của Ricardo Rules không yêu cầu — bỏ qua để giữ đơn giản
- Parameter optimization / backtest
- Hedge mode

---

## 3. Signal Pseudocode

```
evaluate(symbol, interval, klines):

  // Guard: không đủ data
  MIN_CANDLES = atr_period + 2  // closed ATR window + forming bar ignored
  if len(klines) < MIN_CANDLES:
    return Signal(None)

  // Chỉ dùng candles đã đóng; klines.back() có thể đang hình thành
  closedKlines = klines[0 : size-1]
  if len(closedKlines) < atr_period + 1:
    return Signal(None)

  // Tính ATR tại kEval, không đưa forming bar vào ATR
  atr = lastATR(closedKlines, period=atr_period)
  if atr <= 0:
    return Signal(None)

  // Xác định bars
  kEval    = closedKlines[-1]   // last fully closed candle (entry bar)
  setupBar = closedKlines[-2]   // bar trước kEval (setup bar)

  // Long condition: close vượt high của setup bar
  if kEval.close > setupBar.high:
    raw_conf = (kEval.close - setupBar.high) / atr
    confidence = clamp(raw_conf, 0.0, 1.0)
    initial_stop = oneTickBelow(min(setupBar.low, kEval.low), symbol)
    return Signal(
      direction          = Long,
      confidence         = confidence,
      atr                = atr,
      initial_stop_price = initial_stop,
      exit_policy        = SwingTrailing(
                             lookback = swing_lookback,
                             stop_side = SwingLow,
                             fixed_take_profit = None
                           ),
      reason             = interval + " Ricardo breakout long: close=" + kEval.close
                   + " > prev_high=" + setupBar.high
                   + " | initial_stop=" + initial_stop
    )

  // Short condition: close di dưới low của setup bar
  if kEval.close < setupBar.low:
    raw_conf = (setupBar.low - kEval.close) / atr
    confidence = clamp(raw_conf, 0.0, 1.0)
    initial_stop = oneTickAbove(max(setupBar.high, kEval.high), symbol)
    return Signal(
      direction          = Short,
      confidence         = confidence,
      atr                = atr,
      initial_stop_price = initial_stop,
      exit_policy        = SwingTrailing(
                             lookback = swing_lookback,
                             stop_side = SwingHigh,
                             fixed_take_profit = None
                           ),
      reason             = interval + " Ricardo breakout short: close=" + kEval.close
                   + " < prev_low=" + setupBar.low
                   + " | initial_stop=" + initial_stop
    )

  return Signal(None)
```

---

## 4. Config

```json
{
  "name": "Ricardo Rules",
  "type": "ricardo_rules",
  "intervals": ["1d", "4h", "1h", "30m"],
  "scan_interval_seconds": 900,
  "max_hold_duration_seconds": 86400,
  "risk_pct": 0.01,
  "sl_multiplier": 1.5,
  "takeProfitPercent": 0.0,
  "tp_multiplier": 0.0,
  "min_notional": 1.0,
  "atr_period": 14,
  "min_confidence": 0.0,
  "params": {
    "initial_stop_policy": "setup_entry_extreme",
    "exit_policy": "swing_trailing",
    "fixed_take_profit": false,
    "swing_lookback": 3
  }
}
```

---

## 5. Edge Cases

| Tình huống | Xử lý |
|---|---|
| `klines.size() < atr_period + 2` | Return `Signal(None)` ngay |
| `closedKlines.size() < atr_period + 1` | Return `Signal(None)` — không đủ closed-only ATR |
| `atr <= 0` | Return `Signal(None)` — engine sẽ skip |
| Engine chưa hỗ trợ `initial_stop_price` / `SwingTrailing` | Không enable strategy trong production; fallback ATR SL/TP làm sai Ricardo Rules |
| Chưa có swing point xác nhận sau entry | Giữ current stop, không dời trailing stop |
| `setupBar.high == setupBar.low` (doji bar) | Logic vẫn đúng — nếu close vượt high/dưới low của doji thì signal hợp lệ |
| `kEval.close == setupBar.high` | Không phá vỡ (cần strictly greater) → `Signal(None)` |
| `kEval.close == setupBar.low` | Không phá vỡ (cần strictly less) → `Signal(None)` |
| Long và Short đồng thời đúng | Không thể xảy ra — `kEval.close` không thể cùng lúc > `setupBar.high` và < `setupBar.low` |
| `raw_conf > 1.0` (breakout rất xa) | Clamp về `1.0` — vẫn valid, engine xử lý bình thường |

---

## 6. Implementation Notes

> Dành cho developer implement `evaluate()`:

- Tạo `closedKlines = klines[0..klines.size()-2]`, rồi dùng `closedKlines.back()` làm `kEval`, `closedKlines[closedKlines.size()-2]` làm `setupBar`
- ATR đã có sẵn tại `src/strategy/indicators/atr.h` — dùng `strategy::indicators::lastAtr(closedKlines, atr_period)`, không gọi trên vector có forming bar
- `signal.atr` PHẢI được điền — engine dùng để sizing/confidence; custom initial stop không lấy từ ATR
- `evaluate()` phải là `const` và không giữ state
- Không encode `initial_stop` trong `reason` để engine parse; phải truyền qua field/plan có type rõ ràng
- `swing_lookback` được parse và truyền vào trailing plan để engine persist trong `TrackedPosition`

---

## 7. Decision Log

| Quyết định | Alternatives | Lý do chọn | Trạng thái |
|---|---|---|---|
| Entry = `close[-1] > high[-2]` (closed candle) | Break intrabar (high[-1] > high[-2]) | Bot chỉ nhận closed candles; close confirmation bảo thủ hơn, ít false signal | Approved |
| ATR = `lastAtr(closedKlines)` | Tính ATR trên toàn bộ `klines` gồm forming bar | Tránh confidence/sizing/SL thay đổi theo candle chưa đóng; giữ đúng nguyên tắc closed-candle evaluation | Approved |
| Timeframes: `["1d", "4h", "1h", "30m"]` | `["4h", "1h", "30m"]` only | Thêm `1d` giữ đúng tinh thần gốc Ricardo Rules (daily bars); 4 TF coverage đầy đủ | Approved |
| Confidence = clamp(breakout_distance / atr, 0.0, 1.0), `min_confidence = 0.0` | Confidence = 1.0 constant; hoặc lọc breakout yếu | Giữ telemetry về strength nhưng không từ chối breakout hợp lệ, đúng "Never refuse an option" | Approved |
| Custom initial stop field | ATR SL fallback; ghi stop vào reason string | Ricardo Rules yêu cầu stop tại setup/entry extreme; reason string không phải contract thực thi | Approved |
| Swing-point trailing stop thật | Generic N-candle trailing; fixed TP | Preserve intent "Let your profits run"; chỉ dời stop theo swing point đã xác nhận | Approved |
| Disable fixed TP | `takeProfitPercent = 20.0` hoặc ATR TP fallback | Fixed TP cap lợi nhuận, mâu thuẫn với "Let your profits run" | Approved |
| Bỏ "neutral bar" setup filter | Yêu cầu setup bar là inside bar | Mô tả tổng quát của Ricardo Rules không yêu cầu; giữ đơn giản, giảm false negatives | Approved |
| Không patch `config.json` ngay | Patch ngay sau skeleton | DLL chưa build — host sẽ báo lỗi `failed creating strategy type=ricardo_rules` | Approved |
