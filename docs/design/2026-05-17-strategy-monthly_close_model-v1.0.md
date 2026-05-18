# Strategy: Monthly Close Model (Adapted for MTF)

**Version:** 1.1
**Date:** 2026-05-17
**Type:** `monthly_close_model`
**Status:** Implemented — targeted verification passed

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.1 | 2026-05-17 | Clarify TF priority, Binance ROI TP semantics, per-TF hold duration, min data, and implementation status |
| 1.0 | 2026-05-17 | Initial design |

---

## 1. Mục Tiêu

Monthly Close Model (1933, Brent Penfold) là strategy momentum thuần túy: so sánh close của "period hiện tại" với "period trước đó". Nếu giá close cao hơn → Long; thấp hơn → Short. Không có indicator, không có filter phức tạp.

**Edge khai thác:** Trend persistence — khi giá đóng cửa cao hơn period trước, xu hướng có khả năng tiếp tục. Đây là dạng *relative time rate of change* đơn giản nhất.

**Adaptation cho MTF bot:** Bản gốc dùng monthly closes. Bot không có interval `1M`, buffer chỉ 201 candles. Strategy được adapted thành **fixed 30-candle period** trên mọi TF — cùng logic, khác time horizon:
- `1d`: ~30 ngày | `4h`: ~5 ngày | `1h`: ~30 giờ | `30m`: ~15 giờ

**MTF priority thực thi:** Engine evaluate từng `(symbol, interval)` độc lập và chỉ giữ một tracked position cho mỗi symbol. Vì vậy **TF đầu tiên mở được lệnh sẽ dominate các TF sau** cho symbol đó. Interval order của strategy này là `4h → 1h → 30m → 1d`: `4h` có thể dominate `1h`, `1h` có thể dominate `30m`, còn `1d` là slow fallback sau các TF intraday.

---

## 2. Understanding Lock

### 2.1 Signal Logic

| Condition | Direction | Confidence |
|---|---|---|
| `close[last] > close[last - period]` | Long | `clamp((current - prev) / prev, 0.0, 1.0)` |
| `close[last] < close[last - period]` | Short | `clamp((prev - current) / prev, 0.0, 1.0)` |
| `close[last] == close[last - period]` | None | 0.0 |

**Confidence = magnitude của price move** qua `period` candles. Cho phép `min_confidence` filter loại bỏ các period chỉ nhích nhẹ.

### 2.2 Indicators Required

| Indicator | Params | Dùng cho |
|---|---|---|
| ATR | period=14 | `signal.atr` — engine dùng để sizing, SL và TP fallback theo `tp_multiplier` |

Không có indicator signal nào — chỉ thuần so sánh giá đóng cửa.

### 2.3 Minimum Data Requirements

- Cần tối thiểu **31 closed candles**: `max(period + 1, atr_period + 1) = max(31, 15)`
- Buffer hiện tại = 201 candles → **đủ** (31 < 201)
- Chỉ đọc **closed candles** (`kline.isClosed == true`)

### 2.4 Assumptions

| Param | Value | Lý do |
|---|---|---|
| `intervals` | `["4h", "1h", "30m", "1d"]` | Priority theo TF đầu tiên có lệnh: 4h > 1h > 30m; 1d là slow fallback |
| `period` | 30 | Candle-count đồng nhất mọi TF — zero-parameter spirit của bản gốc |
| `atr_period` | 14 | Wilder ATR standard |
| `risk_pct` | 0.01 | 1% balance per trade |
| `sl_multiplier` | 1.5 | SL = entry ± 1.5 × ATR |
| `takeProfitPercent` | 20.0 | Binance Futures ROI/PNL%; price distance = `ROI% / leverage` |
| `tp_multiplier` | 3.0 | ATR fallback khi `takeProfitPercent = 0.0` |
| `min_notional` | 1.0 | Khớp default engine |
| `min_confidence` | 0.001 | ~0.1% move — gần như luôn fire, giữ spirit always-in-market của bản gốc |
| `scan_interval_seconds` | 900 | 2× per 30m candle |
| `max_hold_duration_seconds` | 86400 | Fallback time-exit nếu TF không có override |
| `max_hold_duration_by_interval_seconds.4h` | 432000 | 5 ngày, khớp 30 candles trên 4h |
| `max_hold_duration_by_interval_seconds.1h` | 108000 | 30 giờ, khớp 30 candles trên 1h |
| `max_hold_duration_by_interval_seconds.30m` | 54000 | 15 giờ, khớp 30 candles trên 30m |
| `max_hold_duration_by_interval_seconds.1d` | 2592000 | 30 ngày, khớp 30 candles trên 1d |

### 2.5 Non-Goals

- Không implement true stop-and-reverse (engine không native support)
- Không optimize `period` per-timeframe (cần backtest riêng)
- Không dùng monthly calendar aggregation (buffer constraint — 1h/30m không đủ 720 candles)
- Không hỗ trợ hedge mode

---

## 3. Signal Pseudocode

```
evaluate(symbol, interval, klines):

  // Guard: lọc closed candles
  closedKlines = [k for k in klines if k.isClosed]

  // Guard: cần đủ data cho comparison và ATR
  MIN_CANDLES = max(period + 1, atr_period + 1)   // = 31 với period=30, atr_period=14
  if len(closedKlines) < MIN_CANDLES:
    return Signal(None)

  // Tính ATR (engine dùng cho sizing, SL, TP fallback)
  atr = lastATR(closedKlines, period=atr_period)
  if atr <= 0:
    return Signal(None)

  // Lấy giá đóng cửa hiện tại và period trước
  current_close = closedKlines.back().close
  prev_close    = closedKlines[size - 1 - period].close

  // Guard: sanity check
  if prev_close <= 0:
    return Signal(None)

  // Tính magnitude (always positive)
  delta      = (current_close - prev_close) / prev_close
  confidence = clamp(|delta|, 0.0, 1.0)

  // Long condition
  if current_close > prev_close:
    return Signal(Long, confidence, atr,
                  reason="Period+" + period + " close up " + fmt(delta*100, 2) + "%")

  // Short condition
  if current_close < prev_close:
    return Signal(Short, confidence, atr,
                  reason="Period+" + period + " close dn " + fmt(-delta*100, 2) + "%")

  return Signal(None)
```

---

## 4. Config

```json
{
  "name": "Monthly Close Model (Adapted MTF)",
  "type": "monthly_close_model",
  "intervals": ["4h", "1h", "30m", "1d"],
  "scan_interval_seconds": 900,
  "max_hold_duration_seconds": 86400,
  "max_hold_duration_by_interval_seconds": {
    "1d": 2592000,
    "4h": 432000,
    "1h": 108000,
    "30m": 54000
  },
  "risk_pct": 0.01,
  "sl_multiplier": 1.5,
  "takeProfitPercent": 20.0,
  "tp_multiplier": 3.0,
  "min_notional": 1.0,
  "atr_period": 14,
  "min_confidence": 0.001,
  "params": {
    "period": 30
  }
}
```

---

## 5. Edge Cases

| Tình huống | Xử lý |
|---|---|
| `closedKlines.size() < max(period + 1, atr_period + 1)` | Return `Signal(None)` ngay |
| `atr <= 0` | Return `Signal(None)` — engine sẽ skip |
| `prev_close <= 0` | Return `Signal(None)` — price sanity check |
| `current_close == prev_close` | Return `Signal(None)` — không có momentum |
| `confidence > 1.0` (e.g., 200% pump) | `clamp()` về 1.0 — tránh out-of-range |
| Candle chưa closed (`isClosed == false`) | Lọc ra trước khi xử lý — tránh dùng partial candle |
| Engine đặt lệnh sau signal closed-candle | `evaluate()` chỉ dùng closed candles; engine có thể dùng `klines.back().close` làm execution reference, kể cả khi candle cuối đang forming |

---

## 6. Implementation Notes

> Dành cho developer implement `evaluate()`:

- Tham khảo pseudocode Section 3 và map từng bước vào code
- **Lọc closed candles trước:** `if (kline.isClosed) closedKlines.push_back(kline)` — xem pattern trong `strategy_donchian_5_20_crossover.cpp`
- Signal direction/confidence phải dựa trên closed candles; execution price do engine lấy từ cache mới nhất (`klines.back().close`) và có thể là forming close
- ATR đã có sẵn tại `src/strategy/indicators/atr.h` — dùng `strategy::indicators::lastAtr()`
- `evaluate()` phải là `const` và **không được giữ state**
- `signal.atr` PHẢI được điền — engine dùng để sizing, SL và TP fallback
- `max_hold_duration_by_interval_seconds` là optional override theo signal timeframe; nếu thiếu key, engine dùng `max_hold_duration_seconds`
- `std::clamp` có sẵn trong `<algorithm>` (C++17+)
- Index access: `closedKlines[closedKlines.size() - 1 - static_cast<size_t>(m_params.period)]`
- Format percent: `(delta * 100.0)` với `std::fixed << std::setprecision(2)`

---

## 7. Decision Log

| Quyết định | Alternatives | Lý do chọn | Trạng thái |
|---|---|---|---|
| Run trên cả 4 TFs với priority `4h → 1h → 30m → 1d` | `1d` first; `1d` only; `1d` real monthly + others return None | TF đầu tiên có lệnh giữ symbol; 4h có thể dominate 1h, 1h có thể dominate 30m | Approved |
| Period = 30 candles cố định cho mọi TF | Calendar-normalized (30d/7d/1d/1d); configurable | Zero-parameter spirit của bản gốc; đơn giản; đồng nhất | Approved |
| Always emit Long/Short; confidence = magnitude | Emit only on crossover | Giữ always-in-market spirit; magnitude-based filtering qua `min_confidence` | Approved |
| `min_confidence` = 0.001 (~0.1%) | 0.5 (default) | 0.5 tương đương 50% move — quá cao cho % price comparison | Approved |
| Lọc `isClosed` candles | Dùng tất cả candles | Tránh tín hiệu sai từ partial candle đang hình thành | Approved |
| TP dùng `takeProfitPercent` theo Binance Futures ROI/PNL% | TP là fixed entry ±20% | Khớp SDK/engine: price distance = `ROI% / leverage` | Approved |
| Per-TF hold duration theo 30-candle horizon | Một `max_hold_duration_seconds=86400` cho mọi TF | 1d/4h/1h/30m có horizon khác nhau; fallback vẫn là 24h nếu thiếu key | Approved |
| Minimum data = `max(period + 1, atr_period + 1)` | `period * 2 + 1` | Logic chỉ cần current close, close cách `period`, và ATR seed | Approved |
