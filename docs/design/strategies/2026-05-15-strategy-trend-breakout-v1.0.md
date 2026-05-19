# Strategy: Trend Breakout Trader

**Version:** 1.1  
**Date:** 2026-05-15  
**Type:** `trend_breakout`  
**Status:** Implemented with generic engine trailing stop support

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.1 | 2026-05-15 | Clarified 20-candle 4H breakout execution model; moved trailing stop into generic `TrailingStopController`; added acceptance gates |
| 1.0 | 2026-05-15 | Initial design |

---

## 1. Mục Tiêu

Trend Breakout Trader là trend-following strategy lấy cảm hứng từ *Random Trend Trader* trong sách *The Universal Tactics of Successful Trend Trading* (Brent Penfold, 2020). Strategy khai thác **fat tails** bằng cách cắt lỗ ngắn và để lệnh thắng chạy dài.

Thiết kế này cố ý trade **Donchian breakout 20 candle trên timeframe 4H**:

1. **Entry signal:** close của candle 4H đã đóng gần nhất phá lên high cao nhất hoặc phá xuống low thấp nhất của 20 candle 4H liền trước.
2. **Execution:** engine mở market order ở giá hiện hành khi scan xử lý signal. Signal được tạo từ candle đã đóng; fill thực tế có thể lệch với close của candle signal.
3. **Exit:** initial SL theo ATR, sau đó trailing stop được engine quản lý bằng module generic `TrailingStopController`.

Triết lý cốt lõi: **entry xác định hướng, exit management tạo edge**.

---

## 2. Signal Logic

| Condition | Direction | Confidence |
|---|---|---|
| `interval == "4h"` và `close(k_eval) > max(high của 20 candle 4H trước k_eval)` | Long | 1.0 |
| `interval == "4h"` và `close(k_eval) < min(low của 20 candle 4H trước k_eval)` | Short | 1.0 |
| Không thỏa điều kiện nào | None | 0.0 |

`k_eval = klines[size - 2]`: candle 4H đã đóng gần nhất theo giả định runtime hiện tại, trong đó `klines.back()` có thể là candle đang hình thành. Plugin bỏ qua mọi interval khác `4h` để tránh config sai làm strategy chạy sai timeframe.

### Donchian Window

| Field | Value |
|---|---|
| Breakout timeframe | `4h` |
| Breakout period | `20` candles |
| Evaluation candle | `klines[size - 2]` |
| Lookback range | `[size - 2 - 20, size - 2)` |
| Minimum candles | `22` |

---

## 3. Indicators Required

| Indicator | Params | Dùng cho |
|---|---|---|
| ATR | `period=14` | `signal.atr`, engine dùng để sizing và initial SL |
| Donchian Channel | `period=20`, `interval=4h` | Xác định breakout level |

Donchian Channel được tính trực tiếp bằng loop trên `klines`, không cần thư viện indicator riêng.

---

## 4. Execution Model

Strategy chỉ phát signal. Engine chịu trách nhiệm:

- lấy `signal.atr` để sizing;
- mở market order;
- đặt initial SL theo `entry ± sl_multiplier × ATR`;
- đặt TP theo Binance Futures ROI/PNL% qua `takeProfitPercent` (`price move % = takeProfitPercent / leverage`); khi `takeProfitPercent = 0.0` fallback về `entry ± tp_multiplier × ATR`;
- quản lý trailing stop qua `TrailingStopController`;
- đóng lệnh theo `max_hold_duration_seconds` nếu các exit khác không fire.

Điểm quan trọng: signal dùng candle đã đóng để tránh bias, nhưng market entry dùng giá hiện hành tại thời điểm engine xử lý signal. Backtest phải mô phỏng entry ở candle kế tiếp hoặc giá sau signal, không được fill tại chính close của `k_eval` nếu dữ liệu không cho phép.

---

## 5. Config

```json
{
  "name": "Trend Breakout Trader",
  "type": "trend_breakout",
  "intervals": ["4h"],
  "scan_interval_seconds": 14400,
  "max_hold_duration_seconds": 604800,
  "risk_pct": 0.01,
  "sl_multiplier": 1.5,
  "tp_multiplier": 20.0,
  "takeProfitPercent": 20.0,
  "min_notional": 1.0,
  "atr_period": 14,
  "min_confidence": 0.5,
  "params": {
    "breakout_period": 20,
    "trailing_enabled": true,
    "trailing_interval": "4h",
    "trailing_candles": 42,
    "trailing_check_interval_seconds": 300
  }
}
```

Plugin copies trailing fields from `params` into `StrategyConfig::trailingStop`. Engine only reads the generic `StrategyConfig` fields; it does not inspect strategy-specific raw params.

---

## 6. Generic Trailing Stop Module

Trailing stop is now an engine feature for any strategy that sets `config().trailingStop.enabled = true`.

Module design: [Generic Trailing Stop Controller](../../archives/design/2026-05-15-generic-trailing-stop-controller-v1.0.md).

| Component | Responsibility |
|---|---|
| `strategy::TrailingStopConfig` | Generic per-strategy trailing configuration |
| `engine::TrackedPosition` | Stores trailing state per open position |
| `engine::TrailingStopController` | Pure decision module: computes whether a stop should move |
| `engine::SignalEngine::processTrailingStops()` | Cancels old SL, places new SL, persists tracker update |
| `engine::PositionTracker::updateStopLoss()` | Updates `slOrderId`, `slClientOrderId`, and `currentTrailLevel` under lock |

### 6.1 StrategyConfig Contract

```cpp
struct TrailingStopConfig {
    bool enabled{false};
    std::string interval;
    int candles{0};
    std::chrono::seconds checkInterval{300};
};
```

### 6.2 Position State

```cpp
struct TrackedPosition {
    bool trailingEnabled{false};
    std::string trailingInterval;
    int trailingCandles{0};
    std::chrono::seconds trailingCheckInterval{0};
    double currentTrailLevel{0.0};
};
```

### 6.3 Trail Calculation

`TrailingStopController` uses only closed candles:

- Long: candidate stop = lowest low of the last `N` closed candles.
- Short: candidate stop = highest high of the last `N` closed candles.
- Long stop only moves upward: `candidate > currentTrailLevel`.
- Short stop only moves downward: `candidate < currentTrailLevel`.
- If `currentTrailLevel == 0`, the first valid candidate may create the initial trailing SL.

### 6.4 Order Replacement Safety

When the stop moves:

1. Cancel known old SL by `slOrderId`; fallback to `slClientOrderId`.
2. If a known SL cannot be canceled, do not place a new SL.
3. If no known SL id exists but `currentTrailLevel > 0`, skip placement to avoid duplicate protective orders.
4. Place new reduce-only protection SL.
5. Persist new `slOrderId`, `slClientOrderId`, and `currentTrailLevel` through `PositionTracker::updateStopLoss()`.

---

## 7. Edge Cases

| Tình huống | Xử lý |
|---|---|
| Interval khác `4h` | Return `Signal(None)` |
| `klines.size() < breakout_period + 2` | Return `Signal(None)` |
| `atr <= 0` | Return `Signal(None)` |
| `breakout_period <= 0` trong config | Fallback về 20 |
| `klines.back()` đang hình thành | Không dùng để phát signal |
| Trailing cache thiếu interval | Không dời stop |
| Chưa đủ `trailing_candles` closed candles | Dùng số closed candles đang có |
| Cancel old SL fail | Không đặt SL mới; retry vòng sau |

---

## 8. Acceptance Gates

Trước khi bật production:

| Gate | Minimum |
|---|---|
| Unit tests | `TrendBreakoutStrategy` phát Long/Short/None đúng cho 20-candle 4H breakout |
| Engine tests | trailing stop chỉ move theo hướng có lợi, bỏ qua candle chưa đóng, update tracker sau khi place SL |
| Backtest | Có fee + slippage; entry tại candle kế tiếp hoặc giá sau signal |
| Risk | Max drawdown, expectancy, trade count, loss streak được report theo symbol và portfolio |
| Walk-forward | Params mặc định không được chọn chỉ từ in-sample |
| Dry run | Chạy testnet/paper ít nhất 2 tuần hoặc đủ số signal tối thiểu trước live |

---

## 9. Decision Log

| Quyết định | Alternatives | Lý do chọn | Trạng thái |
|---|---|---|---|
| Breakout 20 candle trên 4H | 1H, 1D, weekly | User chọn rõ 20 candle 4H; phù hợp crypto medium-term ngắn | Approved |
| Confidence = 1.0 cố định | Confidence theo breakout magnitude | Edge nằm ở exit; variable confidence chưa có backtest basis | Approved |
| Initial SL = 1.5 × ATR | Fixed 1% stop | ATR tự điều chỉnh theo volatility và đã được engine hỗ trợ | Approved |
| TP = 20% mặc định, `20 × ATR` fallback | Không đặt TP | Engine hiện có TP order flow; TP xa để trailing là exit chính | Approved |
| Trailing stop là engine module generic | Logic riêng trong `trend_breakout` | Dùng lại cho mọi strategy, không đổi `IStrategy` ABI, dễ test độc lập | Approved |
| Controller chỉ tính decision, SignalEngine xử lý order | Controller tự gọi Orders | Tách calculation khỏi side effect, giảm coupling và dễ unit test | Approved |
