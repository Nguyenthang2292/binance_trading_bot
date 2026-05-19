# ✅ Total Notional Guard — Design Document

**Version:** 1.0
**Date:** 2026-05-16
**Status:** ✅ DONE - Implemented

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-05-16 | Initial design: TotalNotionalGuard, IOrderCapPort, SignalEngine integration, config section |
| 1.0-review-fix | 2026-05-16 | Fix implementation blockers: use `totalMarginBalance`, prefer `/account` positions, merge tracker fallback, fail closed on missing data, update SignalEngine/Gemini wiring |

---

## 1. Mục Tiêu

Thiết kế module `TotalNotionalGuard` inject vào `SignalEngine::openPosition()` để **block lệnh mới khi tổng notional các lệnh đang mở vượt ngưỡng cấu hình** (tính theo phần trăm `totalMarginBalance`).

**Vấn đề hiện tại:**

- `max_position_notional_x_available_balance: 0.5` chỉ giới hạn **một lệnh đơn lẻ** ≤ 50% available balance. Không có giới hạn nào trên tổng cộng tất cả lệnh.
- `ExposureController` kiểm soát exposure theo **beta-weighted net/gross**, trừu tượng hóa xa khỏi notional thô. Không phải là công cụ đúng để giới hạn "tổng tiền đang đặt cược".
- Hệ quả: với `risk_pct = 1%` và account $20, mỗi lệnh ≈ $1. Không có gì ngăn bot mở 20 lệnh đồng thời (100% tài khoản trong market), vượt xa khẩu vị rủi ro thực tế.
- Khi account scale lên (ví dụ $200), position size scale theo ATR → mỗi lệnh ≈ $2, vấn đề tổng exposure vẫn tồn tại theo cách tương tự.

**Phạm vi thiết kế:**

1. `IOrderCapPort` — interface để inject và mock trong test.
2. `TotalNotionalGuard` — implement IOrderCapPort, tính tổng notional từ account snapshot + tracker fallback và block nếu vượt cap.
3. `NoOpOrderCapPort` — no-op khi feature disabled.
4. Integrate vào `SignalEngine::openPosition()` trước `ExposureController` check.
5. Config section `order_cap` trong `config.json`.

---

## 2. Understanding Lock

### 2.1 Summary

- **Cái gì được xây dựng**: Module `TotalNotionalGuard` gate lệnh mới khi `totalOpenNotional + proposedNotional > totalMarginBalance × max_total_notional_pct`.
- **Tại sao**: Không có giới hạn nào trên tổng notional thô của tất cả lệnh đang mở; ExposureController (beta-weighted) không đảm bảo giới hạn này.
- **Reference balance**: `AccountSnapshot.account.totalMarginBalance` (wallet balance + unrealized PnL từ `/fapi/v2/account`) — phản ánh trạng thái thực của tài khoản và field này tồn tại trong `FuturesAccount`.
- **Data source chính**: `AccountSnapshot.account.positions[i].notional` từ cùng response `/fapi/v2/account` với `totalMarginBalance`, tránh giả định atomic giữa `/account` và `/positionRisk`.
- **Fallback/merge**: Dùng `snapshot.positions` từ `/positionRisk` nếu account positions không đủ, rồi merge thêm `PositionTracker` theo symbol bằng `max(remoteNotional, trackerNotional)` để không double count và không bỏ sót lệnh vừa mở cục bộ.
- **Hành vi khi chạm ngưỡng**: Block hoàn toàn — không scale down, không partial fill.
- **Cấu hình**: `max_total_notional_pct` trong `config.json`, ví dụ `0.5` = 50%.

### 2.2 Ví dụ Cụ thể

| totalMarginBalance | max_total_notional_pct | Cap | Đang mở | Lệnh mới | Kết quả |
|---|---|---|---|---|---|
| $20 | 0.5 | $10 | $9 | $2 | **Block** ($9+$2=$11 > $10) |
| $20 | 0.5 | $10 | $8 | $2 | **Allow** ($8+$2=$10 = $10) |
| $20 | 0.5 | $10 | $8 | $3 | **Block** ($8+$3=$11 > $10) |
| $200 | 0.5 | $100 | $90 | $12 | **Block** ($90+$12=$102 > $100) |
| $200 | 0.5 | $100 | $50 | $12 | **Allow** ($50+$12=$62 < $100) |

### 2.3 Assumptions

| # | Assumption |
|---|---|
| A1 | `AccountSnapshot` luôn có `account` từ `/fapi/v2/account`; `includePositions = true` vẫn được giữ trong `openPosition()` vì ExposureController đang cần `/positionRisk` |
| A2 | `Position::notional` từ Binance là giá trị có thể âm cho short positions — dùng `std::abs(notional)` khi tổng hợp |
| A3 | `snapshot.account.totalMarginBalance` và `snapshot.account.positions` đến từ cùng `/account` response; `snapshot.positions` từ `/positionRisk` là fallback nên không được mô tả là atomic với balance |
| A4 | `TotalNotionalGuard::check()` được gọi trước `IExposurePort::check()` — fail fast trên check đơn giản hơn |
| A5 | `proposedNotional` là `sizing.notional` trước khi ExposureController scale down (consistent với cách ExposureController nhận input) |
| A6 | Config section `order_cap` độc lập với `exposure_control` — hai module song song, không phụ thuộc nhau |
| A7 | Khi `order_cap.enabled = false`, inject `NoOpOrderCapPort` → `check()` luôn trả Allow |
| A8 | Failure mode mặc định `closed`: nếu `TotalNotionalGuard::check()` throw exception hoặc không thể xác định remote/tracker state một cách an toàn → block lệnh để bảo toàn risk guarantee |
| A9 | Bot hiện đặt lệnh One-Way với `PositionSide::Both`; merge theo symbol là đúng trong phạm vi hiện tại. Nếu sau này bật Hedge Mode, key merge phải đổi sang `(symbol, positionSide)` để cộng LONG và SHORT riêng |

### 2.4 Non-Goals

- Không scale down lệnh mới (block hoàn toàn hoặc không làm gì).
- Không thay thế `ExposureController` — hai module hoạt động song song, bổ sung cho nhau.
- Không tự động đóng positions đang mở khi tổng vượt cap (chỉ gate entry, không rebalance).
- Không phân biệt LONG vs SHORT trong tổng (dùng abs notional cho tất cả).
- Không có soft limit / scale down (một ngưỡng block duy nhất).
- Không tracking theo per-strategy (một cap duy nhất cho toàn bộ portfolio).

### 2.5 Non-Functional Requirements

- **Performance**: `check()` đồng bộ, < 0.5ms. Chỉ duyệt remote positions và `PositionTracker::all()` (thường < 50 phần tử), cộng phép so sánh số học. Không I/O.
- **Thread safety**: `TotalNotionalGuard` là const sau construction (không mutable state). `AccountSnapshot` được truyền by const reference; `PositionTracker::all()` dùng locking hiện có của tracker.
- **Testability**: `IOrderCapPort` là pure interface → `MockOrderCapPort` trivial trong `test_signal_engine.cpp`.
- **Reliability**: Exception trong `check()` → `openPosition()` catch, log error, áp dụng `failure_mode` (mặc định `closed` → block lệnh).

---

## 3. Current Project State

| Component | File | Liên quan |
|---|---|---|
| SignalEngine | `src/engine/signal_engine.h/cpp` | `openPosition()` là điểm inject; đã có `IExposurePort&` injection pattern |
| IExposurePort | `src/engine/exposure_controller.h` | Template pattern để thiết kế `IOrderCapPort` |
| AccountSnapshot | `src/account/account_snapshot.h` | `AccountSnapshot::account` là `FuturesAccount`; balance đúng là `account.totalMarginBalance` |
| FuturesAccount / Position | `src/types/account.h` | `FuturesAccount::positions[i].notional`, `Position::notional`, `Position::positionAmt` |
| PositionTracker | `src/engine/position_tracker.h` | Dùng để merge local tracked positions chưa xuất hiện trong remote snapshot |
| SizingPolicy | `src/engine/sizing_policy.h` | `SizingResult::notional` là `proposedNotional` input |
| main.cpp | `src/main.cpp` | Điểm wiring tất cả components |

**Snapshot request hiện tại**: `AccountService::snapshot()` luôn gọi `/fapi/v2/account`, nơi có `totalMarginBalance` và `account.positions`. `includePositions = true` vẫn được set vì ExposureController cần `/positionRisk`; `TotalNotionalGuard` không cần thêm REST call mới.

---

## 4. Design Approaches

### 4.1 Recommended: TotalNotionalGuard qua IOrderCapPort — inject vào SignalEngine (chọn)

Module mới implement interface `IOrderCapPort`, inject vào `SignalEngine` constructor theo port pattern hiện có. Check được gọi trong `openPosition()` sau `calculateSize()` và **trước** `IExposurePort::check()`.

**Ưu điểm:**

- Nhất quán với `IExposurePort`, `IScannerPort`, `IAccountPort`, `IOrdersPort` — không tạo pattern mới.
- Dễ mock trong test.
- `TotalNotionalGuard` hoàn toàn độc lập với `ExposureController` — có thể bật/tắt riêng.
- Fail fast: check đơn giản này chạy trước beta-weighted check phức tạp hơn.

**Nhược điểm:**

- Thêm 1 constructor parameter vào `SignalEngine` — cần update wiring trong `main.cpp`.

### 4.2 Alternative: Inline check trong openPosition() (reject)

Không tạo interface, đặt logic kiểm tra trực tiếp trong `openPosition()` với `if` statement.

Bị từ chối vì không mockable trong test — không thể verify hành vi block/allow theo từng scenario mà không cần live snapshot.

### 4.3 Alternative: Extend ExposureController thêm notional cap (reject)

Thêm `max_total_notional_pct` vào `ExposureConfig` và check thêm trong `ExposureController::check()`.

Bị từ chối vì vi phạm SRP: ExposureController đang làm beta-weighted portfolio balancing — trộn lẫn raw notional cap vào đó làm logic phức tạp hơn và khó maintain hơn. Hai concern khác nhau nên tách riêng.

---

## 5. Module Layout

```
src/
  engine/
    signal_engine.h            # Thêm IOrderCapPort& vào constructor
    signal_engine.cpp          # Gọi m_orderCap.check() trong openPosition() trước exposure check
    order_cap_controller.h     # IOrderCapPort interface + TotalNotionalGuard + NoOpOrderCapPort
    order_cap_controller.cpp   # Implementation: tổng notional từ account snapshot + tracker, so sánh với cap

tests/
  test_order_cap_controller.cpp  # Unit tests cho TotalNotionalGuard logic
  test_signal_engine.cpp         # Extend với MockOrderCapPort
```

---

## 6. Proposed Types

### 6.1 OrderCapConfig

```cpp
namespace engine {

enum class OrderCapFailureMode {
    Closed,  // Block khi check lỗi
    Open     // Allow full size khi check lỗi
};

struct OrderCapConfig {
    bool enabled{true};

    // Tổng notional các lệnh đang mở không được vượt quá:
    // totalMarginBalance × maxTotalNotionalPct
    // Ví dụ: 0.5 = 50% totalMarginBalance
    double maxTotalNotionalPct{0.5};

    // Risk guarantee: nếu check() throw exception thì block lệnh.
    // Đổi sang Open nếu muốn ưu tiên availability hơn risk guarantee.
    OrderCapFailureMode failureMode{OrderCapFailureMode::Closed};
};

} // namespace engine
```

### 6.2 OrderCapResult

```cpp
namespace engine {

enum class OrderCapDecision {
    Allow,  // Tổng notional sau lệnh mới vẫn trong ngưỡng
    Block   // Vượt ngưỡng, từ chối mở lệnh
};

struct OrderCapResult {
    OrderCapDecision decision{OrderCapDecision::Allow};
    std::string reason;  // Human-readable, dùng cho log

    // Snapshot metrics tại thời điểm check (dùng cho logging)
    double totalOpenNotional{0.0};   // Tổng abs(notional) các lệnh đang mở
    double proposedNotional{0.0};    // Notional lệnh mới
    double cap{0.0};                 // totalMarginBalance × maxTotalNotionalPct
    double totalMarginBalance{0.0};  // totalMarginBalance tại thời điểm check
};

} // namespace engine
```

### 6.3 IOrderCapPort Interface

```cpp
namespace engine {

class IOrderCapPort {
public:
    virtual ~IOrderCapPort() = default;

    // Kiểm tra tổng notional trước khi mở lệnh mới.
    // proposedNotional: sizing.notional (USDT), chưa apply scale factor nào.
    // snapshot: có account từ /fapi/v2/account; includePositions=true có thể cung cấp thêm /positionRisk.
    // tracker: local positions đã được SignalEngine track, dùng để không bỏ sót state mới mở.
    virtual OrderCapResult check(
        double proposedNotional,
        const account::AccountSnapshot& snapshot,
        const PositionTracker& tracker
    ) const = 0;

    virtual OrderCapFailureMode failureMode() const = 0;
};

// No-op: dùng khi order_cap.enabled = false
class NoOpOrderCapPort final : public IOrderCapPort {
public:
    OrderCapResult check(
        double proposedNotional,
        const account::AccountSnapshot& snapshot,
        const PositionTracker& tracker
    ) const override
    {
        (void)proposedNotional;
        (void)snapshot;
        (void)tracker;
        return {OrderCapDecision::Allow, "order cap disabled"};
    }

    OrderCapFailureMode failureMode() const override {
        return OrderCapFailureMode::Open;
    }
};

} // namespace engine
```

### 6.4 TotalNotionalGuard

```cpp
namespace engine {

class TotalNotionalGuard final : public IOrderCapPort {
public:
    explicit TotalNotionalGuard(OrderCapConfig config);

    OrderCapResult check(
        double proposedNotional,
        const account::AccountSnapshot& snapshot,
        const PositionTracker& tracker
    ) const override;

    OrderCapFailureMode failureMode() const override {
        return m_config.failureMode;
    }

private:
    static double remotePositionNotional(const Position& pos);

    // Tổng abs(notional) tất cả positions từ account snapshot + tracker fallback.
    // Dùng abs() vì Binance có thể trả notional âm cho short positions.
    static double sumOpenNotional(
        const account::AccountSnapshot& snapshot,
        const PositionTracker& tracker);

    OrderCapConfig m_config;
};

} // namespace engine
```

**Implementation của `check()`:**

`order_cap_controller.cpp` cần include `<algorithm>`, `<cmath>`, `<iomanip>`, `<sstream>`, `<unordered_map>`.

```cpp
namespace {

std::string fmt2(double value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

} // namespace

OrderCapResult TotalNotionalGuard::check(
    double proposedNotional,
    const account::AccountSnapshot& snapshot,
    const PositionTracker& tracker) const
{
    const double totalMarginBalance = snapshot.account.totalMarginBalance;
    const double cap = totalMarginBalance * m_config.maxTotalNotionalPct;
    const double totalOpen = sumOpenNotional(snapshot, tracker);
    const double totalAfter = totalOpen + proposedNotional;

    OrderCapResult result;
    result.totalOpenNotional = totalOpen;
    result.proposedNotional  = proposedNotional;
    result.cap               = cap;
    result.totalMarginBalance = totalMarginBalance;

    if (totalAfter > cap) {
        result.decision = OrderCapDecision::Block;
        result.reason =
            "total notional " + fmt2(totalOpen) +
            " + proposed " + fmt2(proposedNotional) +
            " = " + fmt2(totalAfter) +
            " > cap " + fmt2(cap) +
            " (" + fmt2(m_config.maxTotalNotionalPct * 100.0) +
            "% × totalMarginBalance " + fmt2(totalMarginBalance) + ")";
    } else {
        result.decision = OrderCapDecision::Allow;
        result.reason =
            "total notional " + fmt2(totalOpen) +
            " + proposed " + fmt2(proposedNotional) +
            " = " + fmt2(totalAfter) +
            " <= cap " + fmt2(cap);
    }
    return result;
}

double TotalNotionalGuard::remotePositionNotional(const Position& pos)
{
    if (std::abs(pos.positionAmt) <= 0.0) {
        return 0.0;
    }
    if (std::abs(pos.notional) > 0.0) {
        return std::abs(pos.notional);
    }
    if (std::abs(pos.markPrice) > 0.0) {
        return std::abs(pos.positionAmt * pos.markPrice);
    }
    if (std::abs(pos.entryPrice) > 0.0) {
        return std::abs(pos.positionAmt * pos.entryPrice);
    }
    return 0.0;
}

double TotalNotionalGuard::sumOpenNotional(
    const account::AccountSnapshot& snapshot,
    const PositionTracker& tracker)
{
    std::unordered_map<std::string, double> notionalBySymbol;

    auto addRemotePositions = [&](const std::vector<Position>& positions) {
        for (const auto& pos : positions) {
            const double notional = remotePositionNotional(pos);
            if (notional <= 0.0) {
                continue;
            }
            auto& current = notionalBySymbol[pos.symbol];
            current = std::max(current, notional);
        }
    };

    addRemotePositions(snapshot.account.positions); // same /account payload as totalMarginBalance
    if (snapshot.positions.has_value()) {
        addRemotePositions(*snapshot.positions);    // optional /positionRisk fallback
    }

    for (const auto& pos : tracker.all()) {
        auto& current = notionalBySymbol[pos.symbol];
        current = std::max(current, std::abs(pos.quantity * pos.entryPrice));
    }

    double total = 0.0;
    for (const auto& [_, notional] : notionalBySymbol) {
        total += notional;
    }

    return total;
}
```

---

## 7. Data Flow

### 7.1 openPosition() Flow (Updated)

```
openPosition(symbol, interval, direction, atr, currentPrice, cfg, reason):

  [EXISTING] snapshotReq.includePositions = true
  [EXISTING] snapshot = co_await m_account.snapshot(snapshotReq)
  [EXISTING] balance = snapshot.account.availableBalance
  [NEW/USED] totalMarginBalance = snapshot.account.totalMarginBalance

  [EXISTING] sizing = calculateSize({balance, atr, cfg.riskPct,
                                     cfg.slMultiplier, max(cfg.minNotional, engine.minNotional)},
                                    currentPrice, stepSize)
  if sizing.quantity <= 0:
      co_return error("quantity is zero after sizing")

  [NEW] ── Order Cap Check ──────────────────────────────────────────
  try:
      capResult = m_orderCap.check(sizing.notional, snapshot, m_tracker)
  catch:
      if m_orderCap.failureMode() == OrderCapFailureMode::Closed:
          spdlog::error("[OrderCap] check() threw exception, blocking {} {}", symbol, dir)
          co_return {}
      // failureMode == Open: tiếp tục với Allow
      capResult = {OrderCapDecision::Allow, "check failed, fail-open"}

  if capResult.decision == OrderCapDecision::Block:
      spdlog::warn("[OrderCap] Blocked {} {}: {}", symbol, dir, capResult.reason)
      co_return {}
  ─────────────────────────────────────────────────────────────────

  [EXISTING] ── Exposure Check (ExposureController) ────────────────
  try:
      expResult = m_exposure.check(symbol, direction, sizing.notional,
                                   m_tracker, snapshot, balance)
  catch: ...
  if expResult.decision == Block: co_return {}
  ─────────────────────────────────────────────────────────────────

  [EXISTING] orders.market(...)  → open position
  [EXISTING] orders.limit(...)   → TP
  [EXISTING] orders.protection(...)  → SL
  [EXISTING] m_tracker.add(...)

  [EXISTING] Log exposure metrics
```

**Lý do Order Cap check trước Exposure check:**

- Order Cap là phép tính O(n) đơn giản — không cần beta lookup, không có cache miss.
- Fail fast: nếu tổng notional đã vượt, không cần tính beta-weighted metrics.

### 7.2 Relationship với ExposureController

Hai module hoạt động **song song, độc lập**:

```
Signal mới
    │
    ▼
calculateSize()          → sizing.notional = $2
    │
    ▼
TotalNotionalGuard.check()
    ├─ Block → log + co_return {}     ← "tổng tiền đang đặt cược quá nhiều"
    └─ Allow ─────────────────────────────────────────────────────┐
                                                                  │
    ┌─────────────────────────────────────────────────────────────┘
    ▼
ExposureController.check()
    ├─ Block → log + co_return {}     ← "beta-weighted imbalance quá lớn"
    ├─ ScaleDown → điều chỉnh sizing
    └─ Allow ─────────────────────────────────────────────────────┐
                                                                  │
    ┌─────────────────────────────────────────────────────────────┘
    ▼
orders.market() → mở lệnh
```

| Module | Kiểm soát cái gì | Metric | Hành vi khi chạm ngưỡng |
|---|---|---|---|
| `TotalNotionalGuard` | Tổng tiền thô đang trong market | `Σ abs(notional)` vs `totalMarginBalance × pct` | Block hoàn toàn |
| `ExposureController` | Sự mất cân bằng LONG/SHORT theo beta | Net/Gross beta deviation | ScaleDown hoặc Block |

---

## 8. SignalEngine Integration

### 8.1 Constructor

```cpp
// Trước (hiện tại: ExposureController + GeminiFilter đã được thêm)
SignalEngine(
    IScannerPort& scanner,
    strategy::StrategyRegistry& registry,
    IAccountPort& account,
    IOrdersPort& orders,
    IExposurePort& exposure,
    IGeminiFilterPort& geminiFilter,
    GeminiFilterConfig geminiConfig,
    Config config);

// Sau
SignalEngine(
    IScannerPort& scanner,
    strategy::StrategyRegistry& registry,
    IAccountPort& account,
    IOrdersPort& orders,
    IExposurePort& exposure,
    IOrderCapPort& orderCap,    // [NEW]
    IGeminiFilterPort& geminiFilter,
    GeminiFilterConfig geminiConfig,
    Config config);

// Convenience constructor cũng được cập nhật để test/simple wiring dùng default Gemini disabled:
SignalEngine(
    IScannerPort& scanner,
    strategy::StrategyRegistry& registry,
    IAccountPort& account,
    IOrdersPort& orders,
    IExposurePort& exposure,
    IOrderCapPort& orderCap,
    Config config);
```

### 8.2 Member mới

```cpp
IOrderCapPort& m_orderCap;
```

### 8.3 Wiring trong main.cpp

```cpp
engine::NoOpOrderCapPort noOpOrderCap;
std::unique_ptr<engine::TotalNotionalGuard> orderCapController;
engine::IOrderCapPort* orderCapPort = &noOpOrderCap;

if (orderCapConfig.enabled) {
    orderCapController = std::make_unique<engine::TotalNotionalGuard>(orderCapConfig);
    orderCapPort = orderCapController.get();
}

SignalEngine engine(
    scanner,
    registry,
    account,
    orders,
    *exposurePort,
    *orderCapPort,
    *geminiPort,
    geminiConfig,
    engineConfig);
```

---

## 9. Configuration

```json
{
  "engine": {
    "min_notional": 1.0,
    "max_position_notional_x_available_balance": 0.5,
    "position_check_interval_seconds": 60
  },
  "order_cap": {
    "enabled": true,
    "max_total_notional_pct": 0.5,
    "failure_mode": "closed"
  },
  "exposure_control": {
    "enabled": true,
    "target_net_beta": 0.0,
    "soft_limit_net_beta": 0.5,
    "hard_limit_net_beta": 1.0,
    "max_gross_beta": 3.0,
    "default_beta": 1.0,
    "min_notional_after_scale": 5.0,
    "beta_window_days": 30,
    "failure_mode": "closed"
  }
}
```

**Ý nghĩa các tham số (ví dụ với `totalMarginBalance = $20`):**

| Tham số | Giá trị | Ý nghĩa |
|---|---|---|
| `max_total_notional_pct` | 0.5 | Tổng open notional không vượt $10 (50% × $20) |
| `failure_mode` | `"closed"` | Nếu check() lỗi → block lệnh (bảo toàn risk guarantee) |

**Tương tác giữa ba lớp giới hạn:**

| Giới hạn | Config | Scope | Cơ chế |
|---|---|---|---|
| Per-position max | `max_position_notional_x_available_balance: 0.5` | 1 lệnh đơn lẻ | Clamp trong `calculateSize()` |
| Total raw notional | `order_cap.max_total_notional_pct: 0.5` | Tất cả lệnh cộng lại | Block trong `TotalNotionalGuard` |
| Beta-weighted exposure | `exposure_control.*` | Portfolio balance LONG/SHORT | ScaleDown/Block trong `ExposureController` |

---

## 10. Error Handling

| Scenario | Xử lý |
|---|---|
| `snapshot.positions` là `nullopt` | Không coi là empty portfolio. Guard vẫn dùng `snapshot.account.positions` và merge `PositionTracker`; nếu không xác định được state an toàn thì throw để `failure_mode` quyết định |
| `snapshot.account.positions` rỗng và `tracker` rỗng | Xem là không có open position, cho phép nếu lệnh mới nằm trong cap |
| `snapshot.account.positions` rỗng nhưng `tracker` có position | Tính notional từ `abs(tracker.quantity × tracker.entryPrice)` |
| `totalMarginBalance == 0` | `cap = 0` → mọi lệnh có proposed notional dương đều bị block; log warning riêng |
| `totalMarginBalance < 0` (unrealized loss lớn hơn wallet) | `cap < 0` → block tất cả; đây là hành vi đúng (tài khoản đang lỗ nặng) |
| `TotalNotionalGuard::check()` throw exception | `openPosition()` catch, log error, áp dụng `failure_mode`: `closed` → block; `open` → tiếp tục |
| Position notional âm (short position từ Binance) | `std::abs(pos.notional)` — luôn dùng giá trị tuyệt đối |

---

## 11. Testing Strategy

### Unit Tests: test_order_cap_controller.cpp

| Test case | Kiểm tra gì |
|---|---|
| Allow khi totalOpen + proposed ≤ cap | Happy path |
| Allow khi totalOpen + proposed == cap (boundary) | Boundary condition: bằng đúng ngưỡng thì Allow |
| Block khi totalOpen + proposed > cap | Core block logic |
| Block khi `totalMarginBalance == 0` | Edge case: cap = 0 |
| `snapshot.positions == nullopt` vẫn dùng `snapshot.account.positions` | Không fail-open vì thiếu `/positionRisk` |
| `snapshot.account.positions` thiếu symbol nhưng tracker có symbol đó | Local tracker fallback được cộng vào tổng notional |
| Remote + tracker cùng symbol dùng `max(remote, tracker)` | Không double count và giảm rủi ro undercount khi một nguồn stale nhỏ hơn |
| Dùng abs(notional) cho short positions | Short position có notional âm vẫn tính đúng |
| NoOpOrderCapPort luôn Allow | Verify no-op behavior |

**Fixture ví dụ:**

```cpp
// totalMarginBalance = 20.0, maxTotalNotionalPct = 0.5 → cap = 10.0

account::AccountSnapshot makeSnapshot(double totalMarginBalance,
    std::vector<double> notionals) {
    account::AccountSnapshot snap;
    snap.account.totalMarginBalance = totalMarginBalance;
    for (double n : notionals) {
        Position p;
        p.symbol = "SYM" + std::to_string(snap.account.positions.size());
        p.positionAmt = n < 0.0 ? -1.0 : 1.0;
        p.notional = n;
        snap.account.positions.push_back(p);
    }
    return snap;
}

// Test: totalOpen=9, proposed=2 → Block (9+2=11 > 10)
auto snap = makeSnapshot(20.0, {5.0, 4.0});  // total = 9
engine::PositionTracker tracker;
auto result = guard.check(2.0, snap, tracker);
EXPECT_EQ(result.decision, OrderCapDecision::Block);

// Test: totalOpen=8, proposed=2 → Allow (8+2=10 = 10)
auto snap2 = makeSnapshot(20.0, {4.0, 4.0});  // total = 8
auto result2 = guard.check(2.0, snap2, tracker);
EXPECT_EQ(result2.decision, OrderCapDecision::Allow);

// Test: short position với notional=-5 → abs → tổng=5
auto snap3 = makeSnapshot(20.0, {-5.0});  // short position
auto result3 = guard.check(2.0, snap3, tracker);  // total_open=5, total=7 → Allow
EXPECT_EQ(result3.decision, OrderCapDecision::Allow);
```

### Extension: test_signal_engine.cpp

```cpp
// MockOrderCapPort
class MockOrderCapPort final : public engine::IOrderCapPort {
public:
    engine::OrderCapResult check(
        double,
        const account::AccountSnapshot&,
        const engine::PositionTracker&) const override {
        return m_result;
    }
    engine::OrderCapFailureMode failureMode() const override {
        return engine::OrderCapFailureMode::Closed;
    }

    engine::OrderCapResult m_result{engine::OrderCapDecision::Allow, "mock allow"};
};

// Test: TotalNotionalGuard block → openPosition không gọi orders.market()
// Test: TotalNotionalGuard allow + ExposureController block → vẫn block
// Test: TotalNotionalGuard allow + ExposureController allow → mở lệnh
// Test: TotalNotionalGuard throw exception với failureMode=Closed → block
```

---

## 12. Phased Implementation Plan

### Phase A — TotalNotionalGuard + IOrderCapPort

- Tạo `src/engine/order_cap_controller.h` và `order_cap_controller.cpp`
- Implement `OrderCapConfig`, `OrderCapResult`, `IOrderCapPort`, `TotalNotionalGuard`, `NoOpOrderCapPort`
- `order_cap_controller.cpp` dùng standard library formatting theo pattern repo (`std::ostringstream`) và merge theo symbol bằng `std::unordered_map`; không thêm dependency `fmt`
- Update `CMakeLists.txt`: thêm `order_cap_controller.cpp` vào `LIB_SOURCES`
- Viết `tests/test_order_cap_controller.cpp`
- Không có dependency mới — có thể implement và test bằng `AccountSnapshot`/`PositionTracker` fixture, không cần account/orders live.

### Phase B — SignalEngine Integration

- Update `src/engine/signal_engine.h/.cpp`: thêm `IOrderCapPort& orderCap` parameter vào cả constructor đầy đủ có Gemini và convenience constructor; thêm `m_orderCap` member; gọi `m_orderCap.check(size.notional, snapshot, m_tracker)` trong `openPosition()` trước `m_exposure.check()`
- Update `src/main.cpp`: parse `order_cap` section từ config; instantiate `TotalNotionalGuard` hoặc `NoOpOrderCapPort`; truyền `*orderCapPort` vào `SignalEngine` giữa `*exposurePort` và `*geminiPort`
- Update `tests/test_signal_engine.cpp`: thêm `MockOrderCapPort`, cập nhật mọi constructor call để truyền mock/no-op order cap; thêm test cases cho block/allow/exception scenarios

---

## 13. Decision Log

| Quyết định | Lựa chọn đã xem xét | Lý do chọn | Trạng thái |
|---|---|---|---|
| Reference balance = `AccountSnapshot.account.totalMarginBalance` | `availableBalance`, `walletBalance`, `Balance.marginBalance`, `FuturesAccount::totalMarginBalance` | Phản ánh trạng thái thực (wallet + unrealized PnL) và là field compile được trên `FuturesAccount` hiện tại | Approved |
| Hành vi khi chạm ngưỡng = Block hoàn toàn | Block, ScaleDown, Soft+Hard limit | Đơn giản, predictable, dễ debug; không tạo partial position với kích thước không dự đoán được | Approved |
| Threshold cấu hình được trong config.json | Fix cứng 50% | Nhất quán với pattern của exposure_control; không cần recompile để thay đổi khẩu vị rủi ro | Approved |
| Data source = `AccountSnapshot.account.positions[i].notional` + tracker merge | Chỉ dùng `/positionRisk`, chỉ dùng PositionTracker, kết hợp cả hai | `account.positions` cùng payload với `totalMarginBalance`; tracker merge tránh bỏ sót position vừa mở cục bộ; `/positionRisk` chỉ là fallback bổ sung | Approved |
| Module độc lập `TotalNotionalGuard` qua `IOrderCapPort` | Inline trong openPosition(), extend ExposureController | SRP: raw notional cap và beta-weighted balancing là hai concern khác nhau; port pattern cho phép mock trong test | Approved |
| Order Cap check trước Exposure check | Sau exposure check, song song | Fail fast: O(n) đơn giản hơn beta-weighted calculation; nếu notional đã vượt thì không cần tính beta | Approved |
| Dùng `std::abs(pos.notional)` cho short positions | Chỉ tính LONG notional, dùng signed value | Tổng "tiền đang đặt cược" bao gồm cả LONG và SHORT; short cũng chiếm margin và rủi ro | Approved |
| Failure mode default = `closed` | Luôn fail-open, luôn fail-closed, configurable | Nhất quán với ExposureController; bảo toàn risk guarantee mặc định; user có thể override sang `open` | Approved |
| Boundary condition: `totalAfter == cap` → Allow | Strict less, Strict greater, Less-or-equal | Cho phép lấp đầy đúng ngưỡng; "tối đa 50%" nghĩa là bằng cũng được | Approved |
