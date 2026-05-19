# Strategy: Golden 50/200 Moving Average Crossover (Crypto MTF State Variant)

**Version:** 1.0
**Date:** 2026-05-16
**Type:** `golden_crossover`
**Status:** ✅ DONE - Implemented

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-05-16 | Initial design |

---

## 1. Mục Tiêu

Hiện thực hóa chiến lược Golden Cross / Death Cross kinh điển (Brent Penfold — *The Universal Tactics of Successful Trend Trading*, 2020) dưới dạng crypto MTF state-based plugin. Strategy so sánh SMA(50) và SMA(200) trên closed candles để xác định trend direction và emit Long/Short signal tương ứng.

Edge khai thác: momentum trend-following — khi SMA ngắn hạn vượt lên trên SMA dài hạn (Golden Cross), thị trường thường tiếp tục tăng; khi cắt xuống (Death Cross), thị trường có xu hướng giảm. Hậu tố "Crypto MTF State Variant" phân biệt với nguyên bản daily stop-and-reverse: biến thể này chạy trên 3 timeframes đồng thời và dùng ATR bracket exits thay vì chỉ exit khi crossover ngược chiều.

---

## 2. Understanding Lock

### 2.1 Signal Logic

| Condition | Direction | Confidence |
|---|---|---|
| SMA(50) > SMA(200) — Golden Cross state | Long | `clamp(spread / 0.01, min_confidence, 1.0)` |
| SMA(50) < SMA(200) — Death Cross state | Short | `clamp(spread / 0.01, min_confidence, 1.0)` |
| SMA(50) == SMA(200) (near-impossible với real data) | None | 0.0 |

`spread = |SMA50 - SMA200| / SMA200` — relative distance giữa 2 MA.

Confidence tăng khi 2 MA cách nhau xa hơn: spread 1% → confidence 1.0; spread 0.5% → confidence 0.5.

### 2.2 Indicators Required

| Indicator | Params | Dùng cho |
|---|---|---|
| ATR | period=14 | `signal.atr` — engine dùng cho sizing, SL và TP fallback theo `tp_multiplier` |
| SMA | period=50 (`ma_short`) | Short-term trend momentum |
| SMA | period=200 (`ma_long`) | Long-term trend baseline |

### 2.3 Minimum Data Requirements

- Cần tối thiểu **200 closed candles** để tính SMA(200)
- Runtime buffer = 201 candles, warm-up initial limit = 200 → giữ được 200 closed candles và có room cho current forming candle
- Nếu số closed candles < 200 → return None ngay
- Candle gap (network glitch) → closed count giảm → possible miss signal; acceptable trade-off

### 2.4 Assumptions

| Param | Value | Lý do |
|---|---|---|
| `atr_period` | 14 | Wilder ATR standard |
| `risk_pct` | 0.01 | 1% balance per trade |
| `sl_multiplier` | 1.5 | SL = entry ± 1.5 × ATR |
| `takeProfitPercent` | 20.0 | TP mặc định theo Binance Futures ROI/PNL%; khoảng cách giá = ROI% / leverage |
| `tp_multiplier` | 3.0 | ATR fallback khi `takeProfitPercent = 0.0` |
| `min_notional` | 1.0 | Khớp default hiện tại của engine/SDK |
| `min_confidence` | 0.5 | Floor của confidence — filter signal quá yếu |
| `scan_interval` | 900s | Rescan mỗi 15 phút — 2× per 30m candle |
| `max_hold_duration` | 259200s | 3 ngày — MA crossover là slow strategy; 24h cut quá sớm |
| `ma_short` | 50 | Theo tên strategy; short-term trend indicator |
| `ma_long` | 200 | Theo tên strategy; long-term baseline |
| **Signal mode** | **State-based** | Crossover detection cần 201+ closed candles để so sánh previous/current state; state-based giữ scope v1.0 nhỏ và không cần engine change |
| **SMA type** | **Simple MA** | Nguyên bản Brent Penfold dùng SMA; EMA thêm complexity không có trong spec |
| **Dedup** | **Engine-side** | Khi MA50 > MA200, signal Long mỗi scan — engine chịu trách nhiệm không mở duplicate position |

**Semantic mismatch acknowledged:** Trên 30m TF, SMA(50) = ~25 giờ và SMA(200) = ~100 giờ — không phải "50 ngày" và "200 ngày" như nguyên bản daily. MTF must-have convention bắt buộc giữ 30m; signal quality thấp hơn ở lower TF là expected trade-off.

### 2.5 Non-Goals

- Không tối ưu params (cần backtest riêng trên crypto data)
- Không hỗ trợ hedge mode
- Không implement crossover detection (cần buffer > 200 — defer v2.0)
- Không có sideways/ranging market filter (defer v2.0 sau backtest)
- Không kế thừa kết quả lịch sử của nguyên bản (backtest $1.7M trên daily futures 1980–present)

---

## 3. Signal Pseudocode

```
evaluate(symbol, interval, klines):

  // Filter: chỉ dùng closed candles
  closed = [k for k in klines if k.isClosed]

  // Guard: không đủ data
  MIN_CANDLES = 200
  if len(closed) < MIN_CANDLES:
    return Signal(None)

  // Tính ATR
  atr = lastATR(closed, period=14)
  if atr <= 0:
    return Signal(None)

  // Tính SMA
  ma50  = sum(closed[-50:].close)  / 50
  ma200 = sum(closed[-200:].close) / 200

  // Confidence = relative spread giữa 2 MA
  spread     = abs(ma50 - ma200) / ma200
  confidence = clamp(spread / 0.01, min_confidence, 1.0)

  // Long condition: Golden Cross state
  if ma50 > ma200:
    return Signal(Long, confidence, atr,
      reason="Golden Cross: MA50=" + fmt(ma50)
             + " MA200=" + fmt(ma200)
             + " spread=" + fmt(spread*100, 2dp) + "%")

  // Short condition: Death Cross state
  if ma50 < ma200:
    return Signal(Short, confidence, atr,
      reason="Death Cross: MA50=" + fmt(ma50)
             + " MA200=" + fmt(ma200)
             + " spread=" + fmt(spread*100, 2dp) + "%")

  return Signal(None)  // ma50 == ma200: near-impossible với real data
```

---

## 4. Config

```json
{
  "name": "Golden 50/200 Moving Average Crossover (Crypto MTF State Variant)",
  "type": "golden_crossover",
  "intervals": ["4h", "1h", "30m"],
  "scan_interval_seconds": 900,
  "max_hold_duration_seconds": 259200,
  "risk_pct": 0.01,
  "sl_multiplier": 1.5,
  "tp_multiplier": 3.0,
  "takeProfitPercent": 20.0,
  "min_notional": 1.0,
  "atr_period": 14,
  "min_confidence": 0.5,
  "params": {
    "ma_short": 50,
    "ma_long": 200
  }
}
```

Runtime config must keep `scanner.warmup_initial_limit >= 200` and `scanner.kline_buffer_size >= 201`.

---

## 5. Edge Cases

| Tình huống | Xử lý |
|---|---|
| `closed_klines.size() < 200` | Return `Signal(None)` ngay |
| `atr == 0` | Return `Signal(None)` — engine sẽ skip |
| `ma50 == ma200` (exactly equal) | Return `Signal(None)` — near-impossible với real price data |
| Network gap → ít closed candles hơn mong đợi | Return `Signal(None)` — conservative, chấp nhận miss signal |
| `confidence < min_confidence` | Không xảy ra — clamp có floor = `min_confidence` |
| `ma_short >= ma_long` trong config | `validateConfig()` throw → `createStrategy()` return `nullptr` |

---

## 6. Implementation Notes

> Dành cho developer implement `evaluate()`:

- ATR đã có sẵn tại `src/strategy/indicators/atr.h` — dùng `strategy::indicators::lastAtr()`
- SMA phải tự implement local helper `computeSma()` trong plugin — repo chưa có SMA helper shared
- `evaluate()` phải là `const` và **không được giữ state** — tính lại từ `klines` mỗi lần gọi
- `signal.atr` PHẢI được điền — engine dùng để sizing, SL và TP fallback theo `tp_multiplier`
- Chỉ dùng **closed candles** (`kline.isClosed == true`) để tránh noise từ forming candle
- Confidence formula: `std::clamp(std::abs(ma50 - ma200) / ma200 / 0.01, m_cfg.minConfidence, 1.0)`
- Strategy "always signaling" khi đủ data — engine dedup positions, không cần xử lý trong strategy
- Tham khảo `docs/sdk/writing-a-strategy-plugin.md` cho DLL setup chi tiết

---

## 7. Decision Log

| Quyết định | Alternatives | Lý do chọn | Trạng thái |
|---|---|---|---|
| State-based signal (MA50 > MA200 → luôn Long khi đủ data) | Crossover detection (chỉ signal tại điểm giao) | Crossover detection cần 201+ closed candles để so sánh previous/current state; state-based giữ scope v1.0 nhỏ và không cần engine change | Approved |
| MA(50)/MA(200) periods giữ nguyên theo tên strategy | Scale to day-equivalents (300/1200 với 4h TF) | Day-equivalent vượt buffer hoàn toàn; period-count preserves strategy name và semantics | Approved |
| SMA thay vì EMA | EMA nhạy hơn, ít lag hơn | Brent Penfold original dùng SMA; EMA thêm complexity không có trong spec | Approved |
| `max_hold_duration` = 259200s (3 ngày) | 86400s (24h default) | MA crossover là slow strategy — 24h cut prematurely trong nhiều trades tốt (raised by User Advocate agent) | Approved |
| `confidence = clamp(spread/0.01, min_confidence, 1.0)` | Fixed 1.0; stricter formula với divisor 0.02 | Dynamic confidence phản ánh strength of MA separation; floor đảm bảo không miss valid signal; divisor 0.01 calibrated từ review (S4, U2) | Approved |
| 3 TF: `["4h", "1h", "30m"]`, không thêm `"1d"` | Thêm `"1d"` như Donchian để honor daily origin | Runbook MTF must-have là minimum; 4h là TF cao nhất hợp lý cho crypto adaptation của daily strategy | Approved |
| Không có sideways/ranging market filter | Volume filter, ADX filter | YAGNI; cần backtest trước để xác định threshold phù hợp với crypto | Deferred v2.0 |
| Không implement crossover detection trong v1.0 | Phát hiện Golden/Death Cross event chính xác | Buffer constraint (200 candles exact); state-based faithful đủ cho v1.0 | Deferred v2.0 |
