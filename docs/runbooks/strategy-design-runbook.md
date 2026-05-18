# Agent Runbook: Strategy Design & Plugin Registration

**Version:** 1.1
**Date:** 2026-05-16
**Audience:** AI agents
**Scope:** Từ mô tả sơ khai của user → design doc + code skeleton + config snippet/patch an toàn

**Tài liệu liên quan:**

- `docs/sdk/writing-a-strategy-plugin.md` — ABI contract, build steps, runtime checklist (nguồn gốc các ràng buộc trong Section 6)
- `docs/sdk/template_plugin/` — CMakeLists.txt gốc làm chuẩn cho template Section 4.3

---

## Quick Checklist (dành cho agent đã quen)

- [ ] Phase 1 — Elicit: hỏi 5 câu bắt buộc, tự điền defaults
- [ ] Phase 2 — Understanding Lock: trình bày tóm tắt, chờ user confirm
- [ ] Phase 3 — Tạo design doc tại `docs/design/`
- [ ] Phase 3b — Tạo strategy description file tại `docs/strategies/`
- [ ] Phase 4 — Tạo code skeleton tại `plugins/src/<type>/`
- [ ] Phase 5 — Patch `config.json` sau khi DLL tồn tại, hoặc xuất snippet nếu mới tạo skeleton

---

## 1. Anatomy của một Trading Strategy

Hiểu rõ phân công trách nhiệm trước khi elicit:

| Thành phần | Mô tả | Ai xử lý |
|---|---|---|
| **Signal Logic** | Điều kiện trigger Long / Short / None | Strategy (`evaluate()`) |
| **Indicators** | RSI, EMA, ATR, Bollinger, Volume... | Strategy tự tính từ `klines` |
| **Confidence** | Mức độ tin tưởng signal (0.0–1.0) | Strategy |
| **ATR** | Volatility measure trả về cho engine | Strategy tính, Engine dùng |
| **Direction** | Long / Short / None | Strategy |
| **Reason** | Human-readable log string | Strategy |
| **Filters** | Điều kiện phụ giảm false signal | Strategy (bên trong `evaluate()`) |
| **TP / SL** | TP mặc định theo Binance Futures PNL/ROI% (`price move % = ROI% / leverage`; config key: `takeProfitPercent`); SL = `entry ± sl_multiplier × ATR` | Engine (dùng config + `signal.atr` cho SL/sizing và TP fallback) |
| **Position Sizing** | `max(min_notional, balance × risk / (atr × sl_mult))` | Engine |
| **Timeframe** | Interval scan | Config (`intervals`) |
| **Scan Interval** | Tần suất rescan toàn bộ symbols | Config (`scan_interval_seconds`) |
| **Max Hold Duration** | Time-exit fallback | Config (`max_hold_duration_seconds`) |

> **Nguyên tắc cốt lõi**: Strategy chỉ làm một việc — nhận `(symbol, interval, klines)` và trả về `Signal`. Engine lo toàn bộ phần còn lại (sizing, order, TP/SL, time-exit).

---

## 2. Quy Trình Agent

### Phase 1 — Elicit: Thu thập thông tin từ user

#### 1.1 Bắt buộc hỏi (không có default — hỏi tuần tự, một câu mỗi lần)

| # | Câu hỏi | Ghi chú |
|---|---|---|
| 1 | **Tên strategy** là gì? | Human-readable, ví dụ: "RSI Reversal" |
| 2 | **Type string** cho DLL là gì? | `snake_case`, ví dụ: `rsi_reversal`. Nếu user không biết, gợi ý từ tên |
| 3 | **Signal Long** — điều kiện nào trigger? | Yêu cầu mô tả cụ thể: indicator nào, ngưỡng nào |
| 4 | **Signal Short** — điều kiện nào trigger? (hoặc strategy chỉ Long?) | |
| 5 | **Timeframes** nào để scan? | Must-have: `["4h", "1h", "30m"]`. Nếu user không chắc → dùng default này. Có thể thêm `"1d"` nhưng không được bỏ 3 TF must-have |

#### 1.2 Tự điền với defaults (trình bày là assumptions trong design doc)

| Param | Default | Lý do |
|---|---|---|
| `atr_period` | `14` | Wilder ATR standard |
| `risk_pct` | `0.01` | 1% balance per trade |
| `sl_multiplier` | `1.5` | SL = entry ± 1.5 × ATR |
| Binance Futures PNL/ROI% (`takeProfitPercent`) | `20.0` | TP mặc định theo ROI hiển thị trên Binance Futures; khoảng cách giá = ROI% / leverage |
| `tp_multiplier` | `3.0` | ATR fallback khi Binance Futures PNL/ROI% = `0.0` |
| `min_notional` | `1.0` | Khớp default hiện tại của engine/SDK |
| `min_confidence` | `0.5` | Filter signal yếu |
| `scan_interval` | `900` | Rescan mỗi 15 phút — 2× per 30m candle |
| `max_hold_duration` | `86400` | Time-exit sau 24 giờ |

**Binance Futures PNL/ROI% default:** Engine ưu tiên config key `takeProfitPercent` khi giá trị này `> 0.0`. Giá trị này là PNL/ROI% hiển thị trên Binance Futures, không phải % biến động giá trực tiếp. Với default `20.0` và leverage 20x, TP cách entry 1%: Long = `entry * 1.01`, Short = `entry * 0.99`. `tp_multiplier` vẫn phải có trong config nhưng chỉ được dùng làm ATR fallback khi set `takeProfitPercent = 0.0`.

#### 1.3 MTF Must-Have Convention (bắt buộc với mọi strategy)

Mọi strategy **phải** chạy trên cả 3 timeframe sau:

| TF | Vai trò |
|---|---|
| `4h` | Xác nhận trend trung hạn — giảm false signal |
| `1h` | Signal chính — cân bằng giữa độ nhạy và độ nhiễu |
| `30m` | Timing entry — vào lệnh sớm hơn so với 1h |

**Quy tắc:**

- Engine gọi `evaluate()` độc lập cho mỗi `(symbol, interval)`. Mỗi TF tạo signal riêng.
- Agent **không được** thiết kế strategy với `intervals` chỉ gồm 1 TF, trừ khi user yêu cầu rõ ràng và giải thích lý do.
- Có thể thêm `"1d"` vào `intervals` nếu phù hợp với strategy (ví dụ: signal chậm), nhưng 3 TF trên là sàn tối thiểu.
- `scan_interval_seconds` phải `≤ 900` khi có `"30m"` trong `intervals` — mặc định 900 để scan 2× per 30m candle, không được để lớn hơn 900 nếu chưa được chấp thuận rõ.

#### 1.4 Hỏi thêm nếu user đề cập (không hỏi mặc định)

- **Confidence formula** — nếu user nói signal có độ mạnh yếu khác nhau
- **Filters bổ sung** — volume tối thiểu, trend filter, blacklist symbols
- **Override defaults** — nếu user muốn thay đổi bất kỳ param nào ở trên
- **Params riêng của indicator** — RSI period, EMA period, Bollinger std dev, v.v.

---

### Phase 2 — Understanding Lock: Xác nhận với user

Sau khi thu thập đủ thông tin, trình bày bản tóm tắt theo format sau và **dừng lại chờ confirm**:

```
## Strategy: <Tên Strategy>

**Type:** `<type_string>`
**Intervals:** <intervals>

**Signal Logic:**
- Long khi: <mô tả điều kiện cụ thể>
- Short khi: <mô tả điều kiện cụ thể | "Không trade Short">
- None khi: Không thỏa mãn điều kiện nào

**Indicators sử dụng:** <danh sách>

**Assumptions (sẽ ghi rõ trong design doc):**
- atr_period = 14
- risk_pct = 1% | sl = 1.5× ATR | tp = 3.0× ATR
- scan_interval = 900s | max_hold = 86400s
- min_confidence = 0.5
- <assumption bổ sung nếu có>

**Files sẽ tạo:**
- docs/design/<date>-strategy-<type>-v1.0.md
- docs/strategies/<type_string>.md
- plugins/src/<type>/strategy_<type>.cpp
- plugins/src/<type>/CMakeLists.txt
- config.json (chỉ thêm entry sau khi DLL đã build/copy; nếu chưa thì chuẩn bị snippet)

Xác nhận để tôi viết design document?
```

**Không sang Phase 3 cho đến khi user confirm.**

---

### Phase 3 — Viết Design Document

**File:** `docs/design/YYYY-MM-DD-strategy-<type_string>-v1.0.md`

Dùng template Section 4.1. Điền đầy đủ tất cả sections — không để trống bất kỳ section nào.

---

### Phase 3b — Viết Strategy Description File

**File:** `docs/strategies/<type_string>.md`

Dùng template Section 4.5. File này là tài liệu tham khảo ngắn gọn, dành cho người đọc nhanh — không phải design doc kỹ thuật. Điền đầy đủ tất cả sections.

---

### Phase 4 — Tạo Code Skeleton

Tạo 2 files:

- `plugins/src/<type_string>/strategy_<type_string>.cpp` — Dùng template Section 4.2
- `plugins/src/<type_string>/CMakeLists.txt` — Dùng template Section 4.3

**Thay thế tất cả placeholders** `<...>` trước khi ghi file. Không để lại placeholder nào trong output.

---

### Phase 5 — Patch config.json an toàn

Host chỉ scan plugin DLL trực tiếp trong `plugins/`. Nếu thêm entry vào `strategies[]` khi DLL chưa build/copy xong, startup hoặc `--list-strategies` sẽ báo lỗi `failed creating strategy type=<type>`.

Quy tắc:

- Nếu runbook mới dừng ở **design doc + code skeleton**, chỉ xuất config snippet theo Section 4.4 trong báo cáo, **không patch `config.json` ngay**.
- Nếu strategy đã được implement, build thành công và `strategy_<type>.dll` đã nằm trong `plugins/`, mở `config.json` và thêm entry vào cuối mảng `strategies[]`.
- Nếu user yêu cầu patch trước khi DLL tồn tại, ghi rõ đó là **draft config** và cảnh báo bot chưa load được strategy cho đến khi DLL có thật.

---

## 3. Naming Conventions

| Thứ | Convention | Ví dụ |
|---|---|---|
| Type string | `snake_case` | `rsi_reversal` |
| DLL file | `strategy_<type>.dll` | `strategy_rsi_reversal.dll` |
| Source file | `strategy_<type>.cpp` | `strategy_rsi_reversal.cpp` |
| Class name | `<TypePascalCase>Strategy` | `RsiReversalStrategy` |
| Params struct | `<TypePascalCase>Params` | `RsiReversalParams` |
| Design doc | `YYYY-MM-DD-strategy-<type>-v1.0.md` | `2026-05-15-strategy-rsi-reversal-v1.0.md` |
| Strategy description | `docs/strategies/<type_string>.md` | `docs/strategies/rsi_reversal.md` |
| Plugin source dir | `plugins/src/<type_string>/` | `plugins/src/rsi_reversal/` |

---

## 4. Templates

### 4.1 Template: Design Document

~~~markdown
# Strategy: <Tên Strategy>

**Version:** 1.0
**Date:** <YYYY-MM-DD>
**Type:** `<type_string>`
**Status:** Design — pending implementation

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | <date> | Initial design |

---

## 1. Mục Tiêu

<Mô tả strategy làm gì, dựa trên lý thuyết nào — mean-reversion, momentum, breakout, volatility...>
<Giải thích edge mà strategy khai thác là gì.>

---

## 2. Understanding Lock

### 2.1 Signal Logic

| Condition | Direction | Confidence |
|---|---|---|
| <điều kiện Long cụ thể, ví dụ: RSI(14) < 30> | Long | <giá trị hoặc formula> |
| <điều kiện Short cụ thể> | Short | <giá trị hoặc formula> |
| Không thỏa điều kiện nào | None | 0.0 |

### 2.2 Indicators Required

| Indicator | Params | Dùng cho |
|---|---|---|
| ATR | period=<atr_period> | `signal.atr` — engine dùng cho sizing, SL và TP fallback theo `tp_multiplier` |
| <Indicator 2> | <params> | <mục đích> |

### 2.3 Minimum Data Requirements

- Cần tối thiểu **<N> candles** để tính đủ tất cả indicators
- Buffer hiện tại = 200 candles → **đủ / không đủ khi <điều kiện nào>**

### 2.4 Assumptions

| Param | Value | Lý do |
|---|---|---|
| `atr_period` | 14 | Wilder ATR standard |
| `risk_pct` | 0.01 | 1% balance per trade |
| `sl_multiplier` | 1.5 | SL = entry ± 1.5 × ATR |
| Binance Futures PNL/ROI% (`takeProfitPercent`) | 20.0 | TP mặc định theo ROI hiển thị trên Binance Futures; khoảng cách giá = ROI% / leverage |
| `tp_multiplier` | 3.0 | ATR fallback khi Binance Futures PNL/ROI% = `0.0` |
| `min_notional` | 1.0 | Khớp default hiện tại của engine/SDK |
| `min_confidence` | 0.5 | Filter signal yếu |
| `scan_interval` | 900s | Rescan mỗi 15 phút — 2× per 30m candle |
| `max_hold_duration` | 86400s | 24h time-exit |
| <param riêng> | <value> | <lý do> |

### 2.5 Non-Goals

- Không tối ưu params (cần backtest riêng)
- Không hỗ trợ hedge mode
- <Non-goal khác nếu có>

---

## 3. Signal Pseudocode

```

evaluate(symbol, interval, klines):

  // Guard: không đủ data
  if len(klines) < <MIN_CANDLES>:
    return Signal(None)

  // Tính ATR
  atr = lastATR(klines, period=<atr_period>)
  if atr <= 0:
    return Signal(None)

  // Tính indicators
  <indicator_1> = compute<Indicator1>(klines, period=<N>)
  <indicator_2> = compute<Indicator2>(klines, period=<N>)

  // Long condition
  if <điều kiện Long>:
    confidence = <formula>
    return Signal(Long, confidence, atr, reason="<mô tả ngắn>")

  // Short condition
  if <điều kiện Short>:
    confidence = <formula>
    return Signal(Short, confidence, atr, reason="<mô tả ngắn>")

  return Signal(None)

```

---

## 4. Config

```json
{
  "name": "<name>",
  "type": "<type_string>",
  "intervals": [<intervals>],
  "scan_interval_seconds": <scan_interval>,
  "max_hold_duration_seconds": <max_hold_duration>,
  "risk_pct": <risk_pct>,
  "sl_multiplier": <sl_multiplier>,
  "takeProfitPercent": <binance_futures_pnl_roi_percent>,
  "tp_multiplier": <tp_multiplier>,
  "min_notional": <min_notional>,
  "atr_period": <atr_period>,
  "min_confidence": <min_confidence>,
  "params": {
    <params riêng của strategy nếu có>
  }
}
```

---

## 5. Edge Cases

| Tình huống | Xử lý |
|---|---|
| `klines.size() < MIN_CANDLES` | Return `Signal(None)` ngay |
| `atr == 0` | Return `Signal(None)` — engine sẽ skip |
| <Tình huống đặc thù của strategy> | <Cách xử lý> |

---

## 6. Implementation Notes

> Dành cho developer implement `evaluate()`:

- Tham khảo pseudocode Section 3 và map từng bước vào code
- ATR đã có sẵn tại `src/strategy/indicators/atr.h` — dùng `strategy::indicators::lastAtr()`
- `evaluate()` phải là `const` và **không được giữ state** — tính lại mọi thứ từ `klines` mỗi lần gọi
- `signal.atr` PHẢI được điền — engine dùng để sizing, SL và TP fallback theo `tp_multiplier`
- Tham khảo `docs/sdk/writing-a-strategy-plugin.md` cho DLL setup chi tiết
- Repo hiện chỉ có helper ATR chuẩn. Nếu cần RSI/EMA/Bollinger, phải tự implement trong plugin hoặc thêm module indicator trước khi dùng trong production.
- <Ghi chú quan trọng khác>

---

## 7. Decision Log

| Quyết định | Alternatives | Lý do chọn | Trạng thái |
|---|---|---|---|
| <quyết định 1> | <options khác> | <lý do> | Approved |

~~~

---

### 4.2 Template: Code Skeleton

**File:** `plugins/src/<type_string>/strategy_<type_string>.cpp`

```cpp
// strategy_<type_string>.cpp
// Strategy: <Tên Strategy>
// Design: docs/design/<date>-strategy-<type_string>-v1.0.md
//
// TODO: Implement evaluate() theo pseudocode trong design doc Section 3.
// Tham khảo: docs/sdk/writing-a-strategy-plugin.md

#include "strategy/istrategy.h"
#include "strategy/strategy_config.h"
#include "strategy/indicators/atr.h"

#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// ── Params riêng của strategy ─────────────────────────────────────────────
// Khai báo các params không có trong StrategyConfig chuẩn.
// Ví dụ: rsiPeriod, emaPeriod, bollingerStdDev...

struct <TypePascalCase>Params {
    // TODO: Thêm params ở đây nếu có.
    // Ví dụ:
    // int rsiPeriod{14};
    // double oversoldThreshold{30.0};
    // double overboughtThreshold{70.0};
};

<TypePascalCase>Params parseParams(const nlohmann::json& j) {
    <TypePascalCase>Params p;
    const auto& params = j.contains("params") ? j.at("params") : nlohmann::json::object();
    // TODO: Parse từng param từ JSON.
    // Ví dụ:
    // p.rsiPeriod = params.value("rsi_period", 14);
    (void)params;
    return p;
}

strategy::StrategyConfig parseConfig(const nlohmann::json& j) {
    strategy::StrategyConfig cfg;
    cfg.name            = j.value("name", "<type_string>");
    cfg.type            = j.value("type", "<type_string>");
    cfg.intervals       = j.value("intervals", std::vector<std::string>{"4h", "1h", "30m"});
    cfg.scanInterval    = std::chrono::seconds(j.value("scan_interval_seconds", 900));
    cfg.maxHoldDuration = std::chrono::seconds(j.value("max_hold_duration_seconds", 86400));
    cfg.riskPct         = j.value("risk_pct", 0.01);
    cfg.slMultiplier    = j.value("sl_multiplier", 1.5);
    cfg.tpMultiplier    = j.value("tp_multiplier", 3.0);
    // takeProfitPercent is Binance Futures PNL/ROI%, not direct price move percent.
    cfg.takeProfitPercent = j.value("takeProfitPercent", j.value("take_profit_percent", 20.0));
    cfg.minNotional     = j.value("min_notional", 1.0);
    cfg.atrPeriod       = j.value("atr_period", 14);
    cfg.minConfidence   = j.value("min_confidence", 0.5);
    return cfg;
}

// ── Strategy Implementation ───────────────────────────────────────────────

class <TypePascalCase>Strategy final : public strategy::IStrategy {
public:
    <TypePascalCase>Strategy(strategy::StrategyConfig cfg, <TypePascalCase>Params params)
        : m_cfg(std::move(cfg)), m_params(std::move(params)) {}

    const strategy::StrategyConfig& config() const override {
        return m_cfg;
    }

    strategy::Signal evaluate(
        std::string_view symbol,
        std::string_view interval,
        const std::vector<Kline>& klines) const override
    {
        // ── Guard: không đủ candles ───────────────────────────────────────
        // TODO: Thay <MIN_CANDLES> bằng số candle tối thiểu để tính đủ indicators.
        // Xem design doc Section 2.3.
        constexpr int MIN_CANDLES = <MIN_CANDLES>;
        if (static_cast<int>(klines.size()) < MIN_CANDLES) {
            return {};
        }

        // ── Tính ATR ──────────────────────────────────────────────────────
        // ATR bắt buộc phải có — engine dùng để sizing, SL và TP fallback.
        const double atr = strategy::indicators::lastAtr(klines, m_cfg.atrPeriod);
        if (atr <= 0.0) {
            return {};
        }

        // ── Tính Indicators ───────────────────────────────────────────────
        // TODO: Tính các indicator cần thiết.
        // Xem design doc Section 3 — Pseudocode.
        //
        // Lưu ý: repo hiện chỉ có helper ATR chuẩn.
        // Nếu cần RSI/EMA/Bollinger, hãy tự implement local helper trong plugin
        // hoặc thêm module indicator tương ứng trước khi dùng trong production.

        // ── Điều kiện Long ────────────────────────────────────────────────
        // TODO: Implement Long condition theo design doc Section 2.1.
        //
        // Ví dụ:
        // if (rsi < m_params.oversoldThreshold) {
        //     const double confidence = 1.0 - (rsi / m_params.oversoldThreshold);
        //     return strategy::Signal{
        //         .direction  = strategy::Signal::Direction::Long,
        //         .confidence = confidence,
        //         .atr        = atr,
        //         .reason     = "RSI oversold=" + std::to_string(rsi),
        //     };
        // }

        // ── Điều kiện Short ───────────────────────────────────────────────
        // TODO: Implement Short condition theo design doc Section 2.1.
        // Bỏ qua block này nếu strategy chỉ trade Long.
        //
        // Ví dụ:
        // if (rsi > m_params.overboughtThreshold) {
        //     const double confidence = (rsi - m_params.overboughtThreshold)
        //                               / (100.0 - m_params.overboughtThreshold);
        //     return strategy::Signal{
        //         .direction  = strategy::Signal::Direction::Short,
        //         .confidence = confidence,
        //         .atr        = atr,
        //         .reason     = "RSI overbought=" + std::to_string(rsi),
        //     };
        // }

        (void)symbol;
        (void)interval;
        return {};  // No signal
    }

private:
    strategy::StrategyConfig m_cfg;
    <TypePascalCase>Params m_params;
};

} // namespace

// ── C ABI Exports ─────────────────────────────────────────────────────────
// Bốn hàm này bắt buộc phải có — PluginLoader kiểm tra đủ 4 khi load DLL.

extern "C" {

__declspec(dllexport) strategy::IStrategy* createStrategy(const char* config_json) {
    try {
        const auto j = nlohmann::json::parse(config_json == nullptr ? "{}" : config_json);
        auto cfg    = parseConfig(j);
        auto params = parseParams(j);
        return new <TypePascalCase>Strategy(std::move(cfg), std::move(params));
    } catch (...) {
        return nullptr;
    }
}

__declspec(dllexport) void destroyStrategy(strategy::IStrategy* strategy) {
    delete strategy;
}

__declspec(dllexport) const char* strategyType() {
    return "<type_string>";
}

__declspec(dllexport) const char* pluginVersion() {
    return "1.0.0";
}

} // extern "C"
```

---

### 4.3 Template: CMakeLists.txt

**File:** `plugins/src/<type_string>/CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.16)
project(strategy_<type_string> LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(strategy_<type_string> SHARED
    strategy_<type_string>.cpp
    ../../../src/strategy/indicators/atr.cpp
)

target_include_directories(strategy_<type_string> PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)

target_link_libraries(strategy_<type_string> PRIVATE
    nlohmann_json::nlohmann_json
)
```

> **Lý do include `../../../src/strategy/indicators/atr.cpp`**: `strategy::indicators::lastAtr()` không header-only. Nếu chỉ build `strategy_<type>.cpp`, plugin sẽ compile được nhưng link fail vì thiếu symbol ATR.

> **Lý do dùng `FetchContent` thay vì `find_package`**: Plugin là standalone project không depend vào vcpkg/conan của host bot. `FetchContent` đảm bảo đúng version nlohmann_json bất kể môi trường build. Xem `docs/sdk/template_plugin/CMakeLists.txt` làm chuẩn.

---

### 4.4 Template: config.json Entry

Chỉ thêm vào cuối mảng `strategies[]` trong `config.json` sau khi DLL đã build/copy vào `plugins/`. Nếu DLL chưa tồn tại, xuất snippet này trong báo cáo và ghi rõ là chưa áp vào config thật:

```json
{
  "name": "<name>",
  "type": "<type_string>",
  "intervals": ["4h", "1h", "30m"],
  "scan_interval_seconds": 900,
  "max_hold_duration_seconds": <max_hold_duration>,
  "risk_pct": <risk_pct>,
  "sl_multiplier": <sl_multiplier>,
  "takeProfitPercent": <binance_futures_pnl_roi_percent>,
  "tp_multiplier": <tp_multiplier>,
  "min_notional": <min_notional>,
  "atr_period": <atr_period>,
  "min_confidence": <min_confidence>,
  "params": {
    <params riêng nếu có, hoặc xóa block "params" nếu không cần>
  }
}
```

---

### 4.5 Template: Strategy Description File

**File:** `docs/strategies/<type_string>.md`

```markdown
# <Tên Strategy>

**Type:** `<type_string>`
**Version:** 1.0
**Date:** <YYYY-MM-DD>
**Design doc:** [docs/design/<date>-strategy-<type_string>-v1.0.md](../design/<date>-strategy-<type_string>-v1.0.md)

---

## Tổng Quan

<1–3 câu mô tả strategy làm gì, dựa trên lý thuyết nào (mean-reversion / momentum / breakout / volatility...) và edge mà nó khai thác.>

---

## Điều Kiện Vào Lệnh

| Direction | Điều kiện |
|---|---|
| **Long** | <mô tả điều kiện cụ thể bằng ngôn ngữ tự nhiên, ví dụ: RSI(14) < 30 — thị trường oversold> |
| **Short** | <mô tả điều kiện cụ thể — hoặc ghi "Không trade Short"> |

---

## Indicators Sử Dụng

| Indicator | Params | Vai trò |
|---|---|---|
| ATR | period=<atr_period> | Đo volatility — engine dùng để sizing, SL và TP fallback theo `tp_multiplier` |
| <Indicator 2> | <params> | <mục đích> |

---

## Tham Số Mặc Định

| Param | Giá trị | Ý nghĩa |
|---|---|---|
| `intervals` | <intervals> | Timeframes scan |
| `atr_period` | <atr_period> | Chu kỳ ATR |
| `risk_pct` | <risk_pct> | % balance mỗi lệnh |
| `sl_multiplier` | <sl_multiplier> | SL = entry ± sl × ATR |
| Binance Futures PNL/ROI% (`takeProfitPercent`) | <binance_futures_pnl_roi_percent> | TP mặc định theo ROI hiển thị trên Binance Futures; khoảng cách giá = ROI% / leverage |
| `tp_multiplier` | <tp_multiplier> | ATR fallback khi Binance Futures PNL/ROI% = `0.0` |
| `min_confidence` | <min_confidence> | Ngưỡng lọc signal yếu |
| `scan_interval_seconds` | <scan_interval> | Tần suất rescan |
| `max_hold_duration_seconds` | <max_hold_duration> | Time-exit tự động |
| <param riêng> | <value> | <ý nghĩa> |

---

## Điều Kiện Thị Trường Phù Hợp

<Mô tả ngắn: strategy hoạt động tốt nhất trong điều kiện nào — ví dụ: ranging market, trending market, high volatility, v.v.>

---

## Giới Hạn & Cảnh Báo

- <Giới hạn 1 — ví dụ: hiệu suất kém trong trending market mạnh>
- <Giới hạn 2>
- Params mặc định chưa được backtest — cần optimize trước khi dùng production.

```

---

## 5. File Output Checklist

Agent phải tạo/sửa/chuẩn bị đúng các artifacts sau. Kiểm tra từng item trước khi báo cáo hoàn thành:

| # | File | Hành động |
| --- | --- | --- |
| 1 | `docs/design/YYYY-MM-DD-strategy-<type>-v1.0.md` | Tạo mới |
| 2 | `docs/strategies/<type_string>.md` | Tạo mới |
| 3 | `plugins/src/<type>/strategy_<type>.cpp` | Tạo mới |
| 4 | `plugins/src/<type>/CMakeLists.txt` | Tạo mới |
| 5 | `config.json` | Thêm entry vào `strategies[]` sau khi DLL tồn tại; nếu chưa có DLL, chỉ xuất snippet chưa áp |

**Sau khi hoàn thành, thông báo cho user:**

```text
Đã tạo xong:
- Design doc: docs/design/{date}-strategy-{type}-v1.0.md
- Strategy description: docs/strategies/{type_string}.md
- Code skeleton: plugins/src/{type}/strategy_{type}.cpp
- CMake skeleton: plugins/src/{type}/CMakeLists.txt
- Config snippet: đã chuẩn bị entry "{name}" cho strategies[]

Bước tiếp theo (developer):
1. Implement evaluate() theo pseudocode trong design doc Section 3
2. Configure build (Windows, MSVC):
   cmake -S plugins/src/<type> -B build/<type> -G "Visual Studio 17 2022" -A x64
3. Build Release:
   cmake --build build/<type> --config Release
4. Copy DLL vào plugins/ (thủ công nếu không có POST_BUILD):
   copy build/<type>/Release/strategy_<type>.dll plugins/
5. Patch config.json nếu chưa áp:
   thêm entry "<name>" vào `strategies[]`
6. Verify load — chạy bot với flag --list-strategies:
   bot.exe --list-strategies
   → Phải thấy "<name>" trong output trước khi restart thực sự
7. Restart bot để bắt đầu trading với strategy mới
```

---

## 6. Ràng Buộc Quan Trọng (Không Được Vi Phạm)

Nguồn: `docs/sdk/writing-a-strategy-plugin.md` Section 1 + 4.

### 6.1 ABI Contract

| Ràng buộc | Chi tiết |
| --- | --- |
| `evaluate()` phải `const` | Không lưu state, không side effects |
| Không STL types qua ABI boundary | Chỉ dùng `const char*`, `IStrategy*`, void |
| `createStrategy()` phải catch tất cả exceptions | Trả `nullptr` khi lỗi — không throw qua boundary |
| `destroyStrategy()` phải paired với `createStrategy()` | Không dùng `delete` từ phía caller |
| `strategyType()` phải stable và khớp `config.json` `"type"` | Host match DLL bằng string này — đổi là mất load |
| `evaluate()` không được gọi REST/WebSocket | Chỉ đọc từ `klines` được truyền vào |

### 6.2 Runtime Checklist (SDK Section 4 — agent điền vào design doc)

Trước khi agent tạo design doc, phải verify đủ 4 điều kiện sau sẽ được implement:

| # | Điều kiện | Nếu vi phạm |
| --- | --- | --- |
| 1 | `strategyType()` trả về cùng string với `strategies[].type` trong config.json | DLL không được load |
| 2 | `config().intervals` không rỗng | Strategy bị bỏ qua khi build WorkQueue |
| 3 | `evaluate()` trả về `Signal::Direction::None` khi không có signal | Engine có thể mở lệnh sai |
| 4 | `signal.atr > 0` khi có signal | Engine skip lệnh — không trade dù có signal |
| 5 | `config().intervals` phải chứa `"4h"`, `"1h"`, `"30m"` | Vi phạm MTF must-have convention (Section 1.3) |
| 6 | `scan_interval_seconds ≤ 900` khi `intervals` có `"30m"` | Scan chậm hơn convention 2× per 30m candle → tăng rủi ro bỏ lỡ signal trên TF thấp nhất |

### 6.3 WorkQueue Coverage Warning

Current `WorkQueue` scheduling uses zip `strategy + symbol` coverage, not exhaustive strategy-major coverage.

- Each symbol is evaluated by only one strategy per scan cycle.
- Signal coverage is not exhaustive: a strategy can miss a symbol entirely while the symbol universe and strategy order stay stable.
- Example: with `strategies=[TrendBreakout, Gartley]` and sorted symbols `[BTCUSDT, ETHUSDT]`, `BTCUSDT` is evaluated only by `TrendBreakout`, and `ETHUSDT` is evaluated only by `Gartley` each cycle.
- When `strategies.size() > symbols.size()`, some strategies are not scheduled in that cycle. The engine must emit a warning so this starvation is observable.
- For production strategies that require every strategy to evaluate every symbol, do not use the current zip scheduler without adding a `strategy_major` or `symbol_major` policy.

---

## 7. Decision Log

| Quyết định | Alternatives | Lý do chọn | Trạng thái |
| --- | --- | --- | --- |
| Agent tự điền defaults, trình bày là assumptions | Hỏi mọi param | Giảm friction; params kỹ thuật ít thay đổi và đã có giá trị tốt | Approved |
| `evaluate()` là placeholder với TODO | Agent implement logic thực | Agent không đủ context để đảm bảo correctness của signal logic | Approved |
| Không patch `config.json` trước khi DLL tồn tại | Patch ngay sau khi tạo skeleton | Tránh host báo lỗi `failed creating strategy type=<type>` khi plugin chưa build/copy vào `plugins/` | Approved |
| Plugin source tại `plugins/src/<type>/` | Separate repo, `src/strategies/` | Gần với output DLL; không làm phức tạp main build system | Approved |
| Design doc tại `docs/design/` | `docs/strategies/` riêng | Nhất quán với convention đang có trong project | Approved |
| Class trong anonymous namespace | Public class | DLL chỉ expose C ABI — class không cần visibility | Approved |
| Không tạo unit test trong runbook này | Test cùng lúc | Test là bước riêng sau khi implement logic thực | Approved |
