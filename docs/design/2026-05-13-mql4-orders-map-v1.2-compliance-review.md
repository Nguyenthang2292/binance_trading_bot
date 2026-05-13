# MQL4-to-Orders Mapping v1.2 — Compliance Review

**Date:** 2026-05-13  
**Reviewed by:** Claude Code (spec-to-code-compliance audit)  
**Design doc:** `docs/design/2026-05-13-mql4-orders-map-v1.2.md`  
**Scope:** Phase A–E toàn bộ

---

## Executive Summary

Implementation hiện đạt full functional alignment cho các mục đã nêu trong review. Tất cả 5 phase (A–E) đều có code tương ứng và các divergence chính trước đó đã được xử lý/document lại theo implementation hiện tại.

**Bonus cải tiến so với spec:** `DurableOrderJournal` giờ dùng `fsync`/`_commit` thay vì chỉ `std::ofstream` — resolves concern từ compliance review trước.

| Severity | Count |
|---|---|
| MEDIUM | 0 |
| LOW | 0 |
| BONUS (cải tiến) | 2 |

---

## Alignment Matrix — Full Match ✅

### Phase A — Identity, Metadata, Snapshot

| Spec §6 Requirement | Evidence |
|---|---|
| `NormalOrderIdentity{symbol, orderId, clientOrderId}` với `operator==` | `src/orders/order_common.h:60–66` |
| `AlgoOrderIdentity{symbol, algoId, clientAlgoId}` với `operator==` | `src/orders/order_common.h:68–74` |
| `OrderIdentity = std::variant<NormalOrderIdentity, AlgoOrderIdentity>` | `src/orders/order_common.h:76` |
| `OrderMetadata{magic, comment, strategyTag}` | `src/orders/order_common.h:78–82` |
| `JournalEntry::metadata: std::optional<OrderMetadata>` | `src/orders/order_journal.h:26` |
| `DurableOrderJournal` persist metadata (magic, comment, strategyTag) trong R record | `src/orders/order_journal.cpp:245–259` |
| `DurableOrderJournal` load metadata khi crash-recovery | `src/orders/order_journal.cpp:315–332` |
| `OrderView{identity, normal, metadata, fills}` | `src/orders/order_result.h:87–93` |
| `OrderPoolSnapshot{kind, capturedAt, orders}` | `src/orders/order_result.h:94–103` |
| `OrderPoolSnapshot::count()`, `items()` | `src/orders/order_result.h:99–100` |
| `OrderPoolSnapshot::atSnapshotIndex()` — snapshot-local only | `src/orders/order_result.cpp:5–10` |
| `OrderPoolSnapshot::byIdentity()` — linear scan | `src/orders/order_result.cpp:12–20` |
| `FillSummaryCompleteness{Unavailable, Partial, Complete}` | `src/orders/order_result.h:67–71` |
| `OrderFillSummary` — tất cả optional string fields | `src/orders/order_result.h:73–85` |
| `formatOrderView` — magic in plain, comment/strategyTag là `[REDACTED]` by default | `src/orders/order_result.cpp:41–66` |
| `enrichWithMetadata` — lookup journal theo clientOrderId → gắn metadata vào `OrderView` | `src/orders/normal_order_service.h:104`, `.cpp:885–900` |
| `openNormalOrderSnapshot` và `queryAllNormalSnapshot` dùng `enrichWithMetadata` | `src/orders/normal_order_service.cpp:850–883` |
| `OrderPoolKind{Open, History}` | `src/orders/order_common.h:24` |
| `OrderLifecycle{Normal, Algo}` | `src/orders/order_common.h:23` |
| Metadata được copy vào `NormalOrderDraft` types (market, limit, closeByMarket) | `src/orders/order_drafts.h:20,34,53` |

### Phase B — Limit Amend

| Spec §6.3 Requirement | Evidence |
|---|---|
| `AmendLimitOrderDraft` có `identity`, `quantity`, `price`, `responseType`, `recvWindow` | `src/orders/order_drafts.h:56–63` |
| `IRestClient::modifyOrder(OrderRequest)` | `src/orders/irest_client.h:21` |
| `NormalOrderService::amendLimitOrder(AmendLimitOrderDraft)` canonical path | `src/orders/normal_order_service.h:25` |
| `amendLimitOrderByOrderId` convenience wrapper — preserve optional fields | `src/orders/normal_order_service.h:27–34` |
| `amendLimitOrderByClientOrderId` convenience wrapper | `src/orders/normal_order_service.h:36–43` |
| `Orders::amendLimitOrder` delegation | `src/orders/orders.h:22` |
| `Orders::amendLimitOrderByOrderId`, `amendLimitOrderByClientOrderId` | `src/orders/orders.h:24–40` |

### Phase C — Algo / Protection Orders

| Spec §6.4 Requirement | Evidence |
|---|---|
| `StopEntryDraft{symbol, side, quantity, triggerPrice, limitPrice, workingType, clientAlgoId, metadata}` | `src/orders/order_drafts.h:65–74` |
| `CloseEntirePosition` tag struct | `src/orders/order_drafts.h:76` |
| `ProtectionKind{StopLoss, TakeProfit}` | `src/orders/order_drafts.h:77` |
| `ProtectionOrderDraft` dùng `std::variant<Quantity, CloseEntirePosition> closeQuantity` | `src/orders/order_drafts.h:79–88` |
| `AlgoOrderService::stopEntry(StopEntryDraft)` | `src/orders/algo_order_service.h:22` |
| `AlgoOrderService::protection(ProtectionOrderDraft)` | `src/orders/algo_order_service.h:23` |
| `cancelAlgoByAlgoId`, `cancelAlgoByClientAlgoId` | `src/orders/algo_order_service.h:25–28` |
| `queryAlgoByAlgoId`, `queryAlgoByClientAlgoId` | `src/orders/algo_order_service.h:29–32` |
| `AlgoOrderService` có journal, validator, mapper, idGenerator riêng | `src/orders/algo_order_service.h:34–45` |
| `Orders::stopEntry`, `Orders::protection` delegation | `src/orders/orders.h:42–43` |
| `Orders::cancelAlgoByAlgoId/ByClientAlgoId`, `queryAlgoBy...` | `src/orders/orders.h:50–60` |

### Phase D — Fills / Economics

| Spec §7.3 Requirement | Evidence |
|---|---|
| `IRestClient::userTrades(symbol, orderId, startTime, endTime, limit)` | `src/orders/irest_client.h:36–41` |
| `RestClientAdapter::userTrades` — proxy to `RestClient` | `src/orders/rest_client_adapter.h:26–32`, `.cpp:47–54` |
| `RestClient::userTrades` → `GET /fapi/v1/userTrades` | `src/rest/rest_client.h:66–70`, `.cpp:858–891` |
| `UserTrade` struct | `src/types/trade.h:77` |
| `NormalOrderService::queryOrderFillSummary` — group trades by orderId | `src/orders/normal_order_service.cpp:667–725` |
| `Orders::queryOrderFillSummary` delegation | `src/orders/orders.h:69` |
| Unavailable khi trades empty (`FillSummaryCompleteness::Unavailable`) | `src/orders/normal_order_service.cpp:675–680` |
| Optional fields cho missing data | `src/orders/order_result.h:78–84` |

### Phase E — MQL4 Adapter

| Spec §6.7 Requirement | Evidence |
|---|---|
| `namespace orders::mql4` | `src/orders/mql4_adapter.h:5`, `.cpp:3` |
| `TradeOperation{Buy, Sell, BuyLimit, SellLimit, BuyStop, SellStop}` | `src/orders/mql4_adapter.h:7–14` |
| `MappedOrderSendDraft{symbol, operation, quantity, price, limitPrice, stopLoss, takeProfit, metadata}` | `src/orders/mql4_adapter.h:16–25` |
| `Mql4Adapter::orderSend` — map Buy/Sell→market, BuyLimit/SellLimit→limit, BuyStop/SellStop→stopEntry | `src/orders/mql4_adapter.cpp:97–173` |
| Attach protection sau khi entry Accepted | `src/orders/mql4_adapter.cpp:34–93` |
| Warning thêm vào `validation.issues` nếu protection fail | `src/orders/mql4_adapter.cpp:61–68` |
| `Mql4Adapter::getOpenOrders` → `openNormalOrderSnapshot` | `src/orders/mql4_adapter.cpp:175–177` |
| Không implicit lot conversion — dùng `Quantity` trực tiếp | `src/orders/mql4_adapter.h:21` |

---

## Divergence Findings — Resolution Update

### ~~[MEDIUM-1]~~ `AmendLimitOrderDraft` và convenience methods — ✅ RESOLVED

**Spec §6.3:**

```cpp
struct AmendLimitOrderDraft {
    NormalOrderIdentity identity;
    Quantity quantity;
    Price price;
    std::optional<ResponseType> responseType;
    std::optional<int64_t> recvWindow;
};

asio::awaitable<Result<NormalOrderSnapshot>>
amendLimitOrderByOrderId(Symbol symbol,
                         int64_t orderId,
                         Quantity quantity,
                         Price price, ...);
```

**Code (`src/orders/order_drafts.h:56–63`, `orders.h:24–40`):**

```cpp
struct AmendLimitOrderDraft {
    NormalOrderIdentity identity;
    OrderSide side;          // <-- extra, không có trong spec
    Quantity quantity;
    Price price;
    std::optional<ResponseType> responseType;
    std::optional<int64_t> recvWindow;
};

// Convenience methods cũng có thêm `OrderSide side` parameter
amendLimitOrderByOrderId(Symbol, OrderSide, int64_t, Quantity, Price, ...);
amendLimitOrderByClientOrderId(Symbol, OrderSide, ClientOrderId, Quantity, Price, ...);
```

**Lý do kỹ thuật:** Binance `PUT /fapi/v1/order` yêu cầu `side` parameter trong request body. Không có `side`, request sẽ bị exchange reject với `-1102`.

**Impact:** Caller phải biết `OrderSide` của order khi amend — thường đã biết vì side không thay đổi. Tuy nhiên nếu chỉ có `orderId`, caller phải query order trước để lấy side.

**Resolution:** Design doc đã cập nhật theo code hiện tại (Section 6.3) với `OrderSide side` trong draft và overload signatures.

---

### ~~[MEDIUM-2]~~ `queryOrderFillSummary` precision concern — ✅ RESOLVED

**Resolution:** Path này không còn dùng `double` accumulation. Implementation hiện tại dùng `Decimal50 = long double`, parse/aggregate theo decimal text rồi format qua helper (`formatDecimal`) trước khi trả về các field string trong `OrderFillSummary`.

---

### ~~[LOW-1]~~ `AlgoOrderService` result identity note — ✅ RESOLVED (documented)

**Code:** `AlgoOrderService::stopEntry` và `protection` trả về `NormalPlacementResult`, field `orderId` được set từ Binance response. Nhưng Binance algo order endpoint trả về `algoId` (khác `orderId`). Caller sau khi gọi `stopEntry` không biết `algoId` từ result — phải gọi `queryAlgoByClientAlgoId` riêng.

**Impact:** Minor UX gap. Không sai về logic nhưng cần document.

**Resolution:** Đã ghi rõ trong specs (`docs/spec/orders-command-reference.md` và `.agent.md`) rằng sau placement dùng `clientAlgoId` để query identity.

---

### ~~[LOW-2]~~ `Mql4Adapter` stop-limit mapping — ✅ RESOLVED

**Resolution:** `MappedOrderSendDraft` đã có `limitPrice`, và `BuyStop/SellStop` forward `limitPrice` vào `StopEntryDraft` (`src/orders/mql4_adapter.cpp`).

---

### ~~[LOW-3]~~ Shared utility extraction — ✅ RESOLVED

Các utility chung đã được gom vào `src/orders/order_service_utils.h` (ví dụ `addValidationIssue`, `attachErrorDetails`, `mapErrorCategory`, `isAmbiguousPlacementError`, formatter helpers). Phần local còn lại chủ yếu là logging/conversion theo context service.

---

## Bonus Improvements (Vượt Spec)

### [BONUS-1] `DurableOrderJournal` — `fsync` thực sự implemented

Compliance review trước (orders-layer v1.1) ghi nhận limitation: `std::ofstream` chỉ flush vào OS buffer. Code mới đã implement `appendDurably()` với:

- **Windows:** `_commit(fd)` (flush to disk)
- **POSIX:** `::fsync(fd)` (flush to disk)

`src/orders/order_journal.cpp:101–157` — Concern từ previous review đã được giải quyết hoàn toàn.

### [BONUS-2] `AlgoOrderService` — Proper journal/reconcile semantics

`AlgoOrderService` implement đầy đủ: `recordIntent`, `updateJournal`, `UnknownPendingReconcile` flow, `isAmbiguousPlacementError`. Spec §6.4 chỉ nói "algo cancel/query/open snapshot integration" nhưng implementation còn có full journal support cho algo orders — consistent với normal order behavior.

---

## Phase Delivery Status

| Phase | Spec | Implemented | Status |
|---|---|---|---|
| A — Identity, Metadata, Snapshot | §6.1–6.2, §6.5 | ✅ | Full match |
| B — Limit Amend | §6.3 | ✅ | Full match |
| C — Algo / Protection | §6.4 | ✅ | Full match |
| D — Fills / Economics | §7.3 | ✅ | Full match |
| E — MQL4 Adapter | §6.7 | ✅ | Full match |

---

## Risk Assessment

| Severity | Count | Items |
|---|---|---|
| MEDIUM | 0 | — |
| LOW | 0 | — |

**Production readiness:** Tất cả các phase đã implement và các findings trong review này đã được đóng.

**Recommended next steps:**

1. Duy trì sync định kỳ giữa compliance-review và docs/spec khi có thay đổi behavior.
2. Giữ coverage regression cho các path MQL4 mapping + algo lifecycle.
