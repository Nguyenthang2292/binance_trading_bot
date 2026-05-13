# Orders Layer v1.1 — Compliance Review

**Date:** 2026-05-13 (updated after fix pass)
**Reviewed by:** Claude Code (spec-to-code-compliance audit)
**Design doc:** `docs/design/2026-05-12-orders-layer-v1.1.md`
**Scope:** Phase 1 Normal Orders

---

## Executive Summary

Ban đầu implementation đạt ~85% Phase 1. Sau các fix pass, tất cả **3 HIGH**, **5 MEDIUM** và các **LOW findings đã review trong tài liệu này** được đánh dấu hoàn tất.

**Trạng thái hiện tại:** Functionally complete cho Phase 1. Ready for Phase 2 architecture.

---

## Alignment Matrix — Full Match ✅

| Spec Section | Evidence |
|---|---|
| §7.1 Type aliases (`Symbol`, `ClientOrderId`, `CorrelationId`, `RawOrderParams`) | `src/orders/order_types.h:16–20` |
| §7.2 Enums (`ResponseType`, `PlacementState`, `OrderErrorCategory` 11 values) | `src/orders/order_types.h:22–37` |
| §7.3 Facade `Orders` — 11 methods, thin delegation | `src/orders/orders.h:14–42`, `orders.cpp:1–56` |
| §7.4 `MarketOrderDraft`, `LimitOrderDraft`, `CloseByMarketDraft` fields | `src/orders/order_types.h:60–100` |
| §7.4 `NormalOrderDraft = std::variant<...>` | `src/orders/order_types.h:100` |
| §7.6 `NormalPlacementResult`, `NormalCancelResult`, `NormalOrderSnapshot`, `BatchPlacementResult` | `src/orders/order_types.h:102–154` |
| §7.7 `OrdersConfig` core fields | `src/orders/order_types.h:158–168` |
| §8 `DecimalString::parse` — rejects scientific, sign, whitespace, NaN/inf | `src/orders/decimal_string.cpp:14–41` |
| §8 `Price`, `Quantity`, `TriggerPrice` aliases | `src/orders/decimal_string.h:23–25` |
| §8 Mapper preserves outbound decimal text | `src/orders/order_mapper.cpp:33,49,67` |
| §9 Hard validation: symbol, quantity, limit price | `src/orders/order_validator.cpp:39–48,130–131` |
| §9 Hard validation: `reduceOnly` forbidden in Hedge Mode | `src/orders/order_validator.cpp:114–119,134–139` |
| §9 Hard validation: `CloseByMarketDraft` requires OneWay mode | `src/orders/order_validator.cpp:149–154` |
| §9 Hard validation: `clientOrderId` format | `src/orders/order_validator.cpp:26–36` |
| §9 Hard validation: batch max 5 | `src/orders/order_validator.cpp:164–166` |
| §9 Advisory validation: Warning + Skipped issues emitted | `src/orders/order_validator.cpp:87–105` |
| §9 `ValidationReport` always present in result | `src/orders/normal_order_service.cpp:301,335,369` |
| §10 Raw param blocked keys + key format validation | `src/orders/order_validator.cpp:55–84` |
| §10 `timestamp`, `signature`, `recvWindow` conditionally blocked | `src/orders/order_validator.cpp:78–83` |
| §11 `OrderJournal` abstract interface (4 methods) | `src/orders/order_journal.h:28–39` |
| §11 `InMemoryOrderJournal` — thread-safe, dual index | `src/orders/order_journal.cpp:79–127` |
| §11 `DurableOrderJournal` — file-append, crash-recovery | `src/orders/order_journal.cpp:129–300` |
| §12.1 Placement flow: generate ID → validate → journal-before-send → REST → update | `src/orders/normal_order_service.cpp:281–313` |
| §12.2 `UnknownPendingReconcile`: network, parse, -1007/-1008, 5xx | `src/orders/normal_order_service.cpp:181–194` |
| §12.2 `Timeout` category cho -1007/-1008 | `src/orders/normal_order_service.cpp:162–164` |
| §12.2 `CanceledBeforeSend` cho network cancellation keyword | `src/orders/normal_order_service.cpp:169–173` |
| §12.3 Default `ACK` + per-draft `responseType` override | `src/orders/order_mapper.cpp:36–37,54–55` |
| §13 `queryAllNormal` 7-day window enforced | `src/orders/normal_order_service.cpp:600–605` |
| §14 Client ID `btb_<ns>_<ts>_<entropy>`, ≤35 chars | `src/orders/order_id_generator.cpp:47–54` |
| §14 Entropy via `RAND_bytes` (32-bit), 8 lowercase hex; regex `\\-` escape | `src/orders/order_id_generator.cpp:14,43–49` |
| §14 Caller-override validated trước khi dùng | `src/orders/normal_order_service.cpp:203–211` |
| §15 Batch per-item result, `std::visit`, journal per-item before send | `src/orders/normal_order_service.cpp:646–681` |
| §16 `requestParams` serialized (excl. `signature`, `timestamp`) | `src/orders/normal_order_service.cpp:214–243` |
| §17 Durable journal invariant enforced trong `recordIntent` | `src/orders/normal_order_service.cpp:249–254` |
| §18 Logging: correlationId, clientId, symbol, endpoint, latency, state, errorCategory, count | `src/orders/normal_order_service.cpp:105–125` |
| §5/§6 `NormalOrderService` là class riêng; `Orders` là thin facade | `src/orders/normal_order_service.h`, `orders.cpp` |
| `IRestClient` + `RestClientAdapter` | `src/orders/irest_client.h`, `rest_client_adapter.h` |

---

## Divergence Findings — Đã Fixed

### ~~[HIGH-1]~~ `NormalOrderService` — ✅ FIXED

`NormalOrderService` tồn tại đầy đủ tại `src/orders/normal_order_service.h / .cpp`. `Orders` chỉ còn 11 delegation method, không chứa business logic:

```cpp
// orders.cpp:3–4
Orders::Orders(IRestClient& rest, OrdersConfig cfg)
    : m_normalService(std::make_unique<NormalOrderService>(rest, std::move(cfg))) {}
```

---

### ~~[HIGH-2]~~ Advisory validation — ✅ FIXED

`addAdvisoryIssues()` được thêm và gọi trong tất cả validate methods:

```cpp
// order_validator.cpp:87–105
void OrderValidator::addAdvisoryIssues(const Symbol& symbol, ValidationReport& report) const {
    if (m_cfg.positionMode == PositionMode::Unknown) {
        addIssue(report, Warning, "position_mode_unknown", ...);
    }
    if (m_cfg.clientIdNamespace.empty()) {
        addIssue(report, Warning, "no_client_id_namespace", ...);
    }
    addIssue(report, Skipped, "exchange_info_unavailable", ...);
}
```

Advisory validation là best-effort — luôn emit Skipped khi không có exchange-info snapshot. Đúng theo spec.

---

### ~~[HIGH-3]~~ `DurableOrderJournal` — ✅ FIXED

`DurableOrderJournal` được implement với file-append log (tab-separated, R/U records). `OrdersConfig` thêm `journalPath`. `NormalOrderService::recordIntent()` kiểm tra durable invariant:

```cpp
// normal_order_service.cpp:249–254
const bool durableConfigured = m_cfg.journalIsDurable
    || dynamic_cast<DurableOrderJournal*>(m_journal.get()) != nullptr;
if (!m_cfg.allowBestEffortJournal && !durableConfigured) {
    return std::unexpected(BinanceError::fromApiResponse(
        -90009, "Durable journal is required when allowBestEffortJournal=false"));
}
```

**Lưu ý cần theo dõi:**

- **Enum encoded as int:** `side`, `type`, `positionSide`, `state` lưu dạng `static_cast<int>`. Nếu thứ tự enum thay đổi, journal files cũ sẽ parse sai.

---

### ~~[MEDIUM-1]~~ `timestamp`/`signature` blocking — ✅ FIXED

Ba keys giờ được xử lý thống nhất trong conditional block:

```cpp
// order_validator.cpp:78–83
if ((k == "recvWindow" || k == "timestamp" || k == "signature") && !m_cfg.allowRawTimestampOverride) {
    addIssue(report, Error, "raw_recvwindow_blocked", ...);
}
```

**Bonus:** Raw param keys giờ được validate format bằng `kRawParamKeyPattern = "^[A-Za-z][A-Za-z0-9_]{0,63}$"`. Thêm `timeInForce`, `reduceOnly`, `newOrderRespType`, `clientAlgoId` vào blocked list.

---

### ~~[MEDIUM-2]~~ `Timeout`/`CanceledBeforeSend` — ✅ FIXED

```cpp
// normal_order_service.cpp:162–178
if (error.code == -1007 || error.code == -1008) return OrderErrorCategory::Timeout;
// ...
if (error.message.find("Operation canceled") != std::string::npos ...) {
    return OrderErrorCategory::CanceledBeforeSend;
}
```

**Lưu ý:** `CanceledBeforeSend` dựa vào string matching trên `error.message`. Nếu Boost.Asio thay đổi error message, detection fail silently. Robust hơn là check `boost::asio::error::operation_aborted` error code.

---

### ~~[MEDIUM-3]~~ `requestParams` empty — ✅ FIXED

`serializeRequestParams()` serialize tất cả `OrderRequest` fields, explicit skip `signature` và `timestamp` (`normal_order_service.cpp:236–241`).

---

### ~~[MEDIUM-4]~~ `OrdersConfig` undocumented fields — ✅ RESOLVED

Tất cả extra fields giờ có mục đích rõ ràng và được sử dụng: `positionMode` (validator), `journal` (inject instance), `journalIsDurable` (được đọc trong constructor để instantiate `DurableOrderJournal`), `journalPath` (path cho file-append journal).

---

### ~~[MEDIUM-5]~~ Regex hyphen UB — ✅ FIXED

Cả hai files đã dùng `"^[A-Za-z0-9._:@/\\-]{1,36}$"` (escaped hyphen).

---

### ~~[LOW-1]~~ Observability — ✅ FIXED

`logPlacement()` log đầy đủ: correlationId, clientId, symbol, endpoint, latencyMs, state, errorCategory, binanceCode, `unknownPendingReconcileCount` (atomic counter tăng mỗi khi state là `UnknownPendingReconcile`).

---

### ~~[LOW-3]~~ Validation order — ✅ FIXED

`prepareMarket/limit/closeByMarket` trong `NormalOrderService` generate client ID trước, set lên draft, rồi mới validate — đúng theo spec §12.1 flow: generate → validate.

---

## Findings Còn Lại — Updated

### ~~[LOW-2]~~ Rate-limit header processing — ✅ FIXED

Header usage hiện đã được parse và đưa vào limiter chain:

- Header parse: `X-MBX-USED-WEIGHT-1M`, `X-MBX-ORDER-COUNT-1M`, `X-MBX-ORDER-COUNT-10S`
  (`src/transport/rate_limit_headers.cpp:45–55`)
- `HttpSession` extract headers từ response và cập nhật `lastUsed*`
  (`src/transport/http_session.cpp:126–138`)
- `RestClient` update `RateLimiter` sau mỗi request signed/public
  (`src/rest/rest_client.cpp:416–444`)

---

### ~~[LOW-4]~~ `order_drafts.h` / `order_result.h` merge vào `order_types.h` — ✅ FIXED

Hiện tại types đã tách lại theo đúng architecture:

- `src/orders/order_drafts.h`
- `src/orders/order_result.h`
- `src/orders/order_common.h`

`src/orders/order_types.h` chỉ còn là aggregator include mỏng (`#include` 3 headers trên).

---

### ~~[LOW-5]~~ Test coverage gaps vs spec §20 — ✅ FIXED

| Spec §20 requirement | Status |
|---|---|
| Journal write xảy ra trước send (ordering guarantee) | Covered (`tests/test_orders.cpp:277–309`) |
| Timeout after send → `UnknownPendingReconcile` | Covered (`tests/test_orders.cpp:148–175`, `215–244`) |
| Durable journal failure blocks placement | Covered (`tests/test_orders.cpp:333–361`) |
| Rate-limit headers passed into limiter | Covered (`tests/test_rate_limiter.cpp:20–40`) |
| `ValidationReport` present trên cả success và failure paths | Covered (`tests/test_orders.cpp:519–555`) |
| `queryAllNormal` rejects ≥7-day windows | Covered (`tests/test_orders.cpp:452–464`) |
| `cancelAllNormal` invokes correct endpoint | Covered (`tests/test_orders.cpp:440–450`) |
| Advisory warnings/skipped present trong result | Covered (`tests/test_orders_validator.cpp:175–202`, + presence on placement result `tests/test_orders.cpp:519–555`) |

---

## Risk Assessment — Sau Fix

| Severity | Trước fix | Sau fix |
|---|---|---|
| HIGH | 3 | 0 |
| MEDIUM | 5 | 0 |
| LOW | 4 | 0 |

**Production readiness:** Phase 1 complete. `DurableOrderJournal` dùng được với hiểu biết về limitation fsync. Advisory validation hoạt động ở best-effort mode. Architecture sẵn sàng cho Phase 2.

**Blocking cho Phase 2:** Không có. `NormalOrderService` tồn tại, `AlgoOrderService` có thể thêm song song.

**Recommended next steps:**

1. Thêm integration test end-to-end cho đường `HttpSession -> RestClient -> RateLimiter` (hiện đã có unit coverage cho parser + limiter).
2. Thêm `fsync` hard guarantee hoặc tài liệu hoá rõ durability semantics theo môi trường vận hành.
3. Duy trì checklist spec-to-code-compliance cho Phase 2 để tránh drift.
