# Binance Futures Orders Layer — Design Document

**Version:** 1.1  
**Date:** 2026-05-12  
**Status:** ✅ DONE - Implemented  
**Disposition:** APPROVED

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-05-12 | Initial approved design |
| 1.1 | 2026-05-12 | Define all result types; add `NormalOrderDraft` variant; add `OrdersConfig`, `ResponseType`, `RawOrderParams`; specify `OrderJournal` interface; concrete client ID spec; add `cancelAllNormal` / `queryAllNormal`; clarify `openNormalOrders` weight; note `RestClient` injection requirement |

---

## 1. Mục Tiêu

Thiết kế lớp `orders` nằm giữa strategy/trading engine và `RestClient` hiện có để đặt, hủy, truy vấn và reconcile lệnh Binance USDⓈ-M Futures một cách rõ ràng hơn so với việc caller tự điền `OrderRequest` generic.

Layer này tập trung trước vào:

- `MARKET` và `LIMIT` normal orders qua REST `/fapi/v1/order`
- batch normal orders qua `/fapi/v1/batchOrders`
- cancel/query/open/all normal orders
- contract API cho algo orders, nhưng triển khai algo orders là phase riêng vì Binance hiện có lifecycle và endpoint riêng cho TP/SL/trailing
- validation trước khi gửi lệnh, nhưng Binance server vẫn là nguồn quyết định cuối cùng
- idempotency và reconcile khi trạng thái gửi lệnh không chắc chắn

---

## 2. Understanding Lock

### 2.1 Summary

- Đang xây dựng một lớp order domain/service cho C++20 Binance USDⓈ-M Futures SDK.
- Người dùng chính là strategy/trading engine trong repo, không phải UI end-user.
- Lớp này phải che bớt chi tiết request REST thô nhưng không được che mất semantics quan trọng của Binance Futures.
- Existing stack là `Boost.Asio` coroutines, `RestClient`, `RateLimiter`, `Signer`, `std::expected<T, BinanceError>` và `simdjson`.
- Normal orders và algo/conditional orders phải được phân biệt vì Binance có endpoint, response, query/cancel và lifecycle khác nhau.
- Trạng thái placement không chắc chắn phải là trạng thái nghiệp vụ rõ ràng, không được coi như reject chắc chắn.
- File này là artifact thiết kế; không triển khai code trong bước này.

### 2.2 Assumptions

- Chỉ thiết kế cho USDⓈ-M Futures, không thiết kế Spot hoặc COIN-M.
- `RestClient` hiện có tiếp tục là lớp transport REST nền tảng.
- V1 ưu tiên normal `MARKET` và `LIMIT`; algo order design được ghi để tránh API sai hướng nhưng có thể implement phase 2.
- Strategy có thể dùng hedge mode hoặc one-way mode; order layer không giả định một mode cố định.
- Production trading cần durable reconcile. Nếu caller không cấu hình journal bền vững, đó là chế độ best-effort và phải explicit opt-in.
- Price/quantity ở boundary order không dùng `double`; outbound string phải giữ đúng giá trị caller đã yêu cầu sau canonicalization.

### 2.3 Open Questions

- Storage backend cụ thể cho durable order journal sẽ là file append-only, SQLite hay adapter do application cung cấp.
- Có cần giữ legacy `BinanceAPI::createOrder(string side, string type, double quantity, ...)` hay deprecate sau khi có `orders` layer.

---

## 3. Tham Chiếu

| Nguồn | URL | Ghi chú |
|---|---|---|
| Binance New Order | <https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/New-Order> | `POST /fapi/v1/order`, order-count headers, `newClientOrderId`, `newOrderRespType` |
| Binance Place Multiple Orders | <https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/Place-Multiple-Orders> | `POST /fapi/v1/batchOrders`, max 5, per-item result |
| Binance New Algo Order | <https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/New-Algo-Order> | `POST /fapi/v1/algoOrder`, TP/SL/trailing |
| Binance Query Order | <https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/Query-Order> | query normal order, retention constraints |
| Binance Cancel Order | <https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/Cancel-Order> | cancel by `orderId` or `origClientOrderId` |
| Binance Cancel All Orders | <https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/Cancel-All-Open-Orders> | `DELETE /fapi/v1/allOpenOrders` |
| Binance All Orders | <https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/All-Orders> | `GET /fapi/v1/allOrders`, query order history |
| Binance Query Algo Order | <https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/Query-Algo-Order> | query algo order, retention constraints |
| Binance Cancel Algo Order | <https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/Cancel-Algo-Order> | cancel by `algoId` or `clientAlgoId` |
| Existing SDK design | `docs/archives/design/2026-05-11-binance-futures-sdk-v1.0.md` | base architecture, REST client, types |

---

## 4. Non-Goals

- Không thay thế `RestClient`; `orders` layer dùng `RestClient`.
- Không implement WebSocket order placement trong v1.
- Không đảm bảo lệnh sẽ được Binance accept chỉ bằng client-side validation.
- Không tự động retry create order mù sau timeout hoặc network failure.
- Không gộp normal order và algo order vào cùng lifecycle giả tạo.
- Không thêm strategy logic như sizing, risk management, stop placement policy.

---

## 5. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│                         Strategy / Trading Engine                    │
├──────────────────────────────────────────────────────────────────────┤
│                                Orders                                │
│  facade: market, limit, closeByMarket, cancel/query/open/all/batch   │
├───────────────────────────────┬──────────────────────────────────────┤
│      NormalOrderService       │        AlgoOrderService (Phase 2)    │
│  MARKET/LIMIT + normal query  │  STOP/TP/TRAILING + algo query/cancel│
├───────────────────────────────┴──────────────────────────────────────┤
│ OrderValidator │ OrderIdGenerator │ OrderJournal │ TimeSync │ Mapper │
├──────────────────────────────────────────────────────────────────────┤
│                        IRestClient (interface)                       │
├──────────────────────────────────────────────────────────────────────┤
│                 RestClient (production implementation)               │
├──────────────────────────────────────────────────────────────────────┤
│                 HttpSession + Signer + RateLimiter                   │
└──────────────────────────────────────────────────────────────────────┘
```

`Orders` là facade nhỏ. Nó không nên chứa toàn bộ logic; logic chính nằm trong service theo lifecycle:

- `NormalOrderService`: normal order lifecycle.
- `AlgoOrderService`: conditional/algo order lifecycle.
- `OrderValidator`: validation chắc chắn và advisory validation.
- `OrderJournal`: ghi intent trước khi gửi để reconcile được sau timeout/crash.
- `OrderIdGenerator`: sinh `newClientOrderId`/`clientAlgoId`.
- `OrderMapper`: chuyển typed draft sang Binance REST params.

`IRestClient` là abstract interface cho `RestClient`. `NormalOrderService` nhận `IRestClient&` để test có thể inject `FakeRestClient` mà không cần kết nối Binance.

---

## 6. Module Layout

Đề xuất thêm thư mục:

```
src/
└── orders/
    ├── orders.h / orders.cpp
    ├── normal_order_service.h / normal_order_service.cpp
    ├── algo_order_service.h / algo_order_service.cpp
    ├── order_drafts.h
    ├── order_result.h
    ├── order_validator.h / order_validator.cpp
    ├── order_mapper.h / order_mapper.cpp
    ├── order_id_generator.h / order_id_generator.cpp
    ├── order_journal.h / order_journal.cpp
    └── decimal_string.h / decimal_string.cpp

tests/
└── orders/
    ├── test_order_drafts.cpp
    ├── test_order_validator.cpp
    ├── test_order_mapper.cpp
    ├── test_order_id_generator.cpp
    └── test_normal_order_service.cpp
```

`src/types/trade.h` có thể giữ các enum dùng chung (`OrderSide`, `OrderType`, `TimeInForce`, `PositionSide`, `WorkingType`) trong giai đoạn đầu. Khi implementation ổn định, có thể tách types domain order sang `src/orders/` nếu cần.

---

## 7. Public API Shape

### 7.1 Type Aliases

```cpp
using Symbol        = std::string;
using ClientOrderId = std::string;
using ClientAlgoId  = std::string;
using CorrelationId = std::string;
using RawOrderParams = std::unordered_map<std::string, std::string>;
```

`Symbol` và `ClientOrderId` dùng `std::string` để tương thích với các type hiện có trong `RestClient`. Wrapper type có thể được thêm sau nếu cần type-safety mạnh hơn.

### 7.2 Enums

```cpp
enum class ResponseType { ACK, RESULT };
```

`ACK` là default. `RESULT` là opt-in vì có thể tăng latency và ambiguity trên timeout (xem Section 11.3).

`OrderSide`, `OrderType`, `TimeInForce`, `PositionSide`, `WorkingType` giữ nguyên từ `src/types/trade.h`.

### 7.3 Facade

```cpp
class Orders {
public:
    Orders(IRestClient& rest, OrdersConfig cfg);

    boost::asio::awaitable<Result<NormalPlacementResult>>
    market(MarketOrderDraft draft);

    boost::asio::awaitable<Result<NormalPlacementResult>>
    limit(LimitOrderDraft draft);

    boost::asio::awaitable<Result<NormalPlacementResult>>
    closeByMarket(CloseByMarketDraft draft);

    boost::asio::awaitable<Result<NormalCancelResult>>
    cancelNormalByOrderId(Symbol symbol, int64_t orderId);

    boost::asio::awaitable<Result<NormalCancelResult>>
    cancelNormalByClientOrderId(Symbol symbol, ClientOrderId clientOrderId);

    // DELETE /fapi/v1/allOpenOrders — cancels all open orders for a symbol.
    // symbol is required; Binance does not support cross-symbol cancel-all.
    boost::asio::awaitable<Result<void>>
    cancelAllNormal(Symbol symbol);

    boost::asio::awaitable<Result<NormalOrderSnapshot>>
    queryNormalByOrderId(Symbol symbol, int64_t orderId);

    boost::asio::awaitable<Result<NormalOrderSnapshot>>
    queryNormalByClientOrderId(Symbol symbol, ClientOrderId clientOrderId);

    // GET /fapi/v1/openOrders — returns current open normal orders.
    // Request weight is 1 with a symbol and 40 when symbol is omitted.
    // Prefer symbol-scoped calls unless the caller intentionally accepts the higher weight.
    boost::asio::awaitable<Result<std::vector<NormalOrderSnapshot>>>
    openNormalOrders(std::optional<Symbol> symbol = std::nullopt);

    // GET /fapi/v1/allOrders — order history with optional time range and limit.
    // Required for reconcile when openNormalOrders no longer contains the order
    // (filled/canceled orders leave the open-orders list immediately).
    // Default limit 500; max 1000. Retention: 90 days.
    // Each request time period must be less than 7 days; reconcile code must chunk wider ranges.
    boost::asio::awaitable<Result<std::vector<NormalOrderSnapshot>>>
    queryAllNormal(Symbol symbol,
                   std::optional<int64_t> startTime = std::nullopt,
                   std::optional<int64_t> endTime   = std::nullopt,
                   int limit = 500);

    boost::asio::awaitable<Result<BatchPlacementResult>>
    batchNormal(std::vector<NormalOrderDraft> drafts);
};
```

Notes:

- Method names include `Normal` when identity/lifecycle could be ambiguous.
- Algo methods must not appear as working facade methods until `AlgoOrderService` exists.
- If an explicit unsupported path is kept for compile compatibility, it must return `Unsupported` and must not route to normal order endpoints.
- `cancelNormalByClientOrderId` maps to Binance `origClientOrderId` query parameter — these names are synonymous at the REST level.

### 7.4 Normal Drafts

```cpp
struct MarketOrderDraft {
    Symbol symbol;
    OrderSide side;
    Quantity quantity;
    PositionSide positionSide = PositionSide::Both;
    std::optional<bool> reduceOnly;
    std::optional<ClientOrderId> clientOrderId;
    std::optional<ResponseType> responseType;   // default ACK from OrdersConfig
    RawOrderParams raw;
};

struct LimitOrderDraft {
    Symbol symbol;
    OrderSide side;
    Quantity quantity;
    Price price;
    TimeInForce timeInForce = TimeInForce::GTC;
    PositionSide positionSide = PositionSide::Both;
    std::optional<bool> reduceOnly;
    std::optional<ClientOrderId> clientOrderId;
    std::optional<ResponseType> responseType;   // default ACK from OrdersConfig
    RawOrderParams raw;
};

struct CloseByMarketDraft {
    Symbol symbol;
    // Side sent to Binance. For one-way mode this must be the side that reduces
    // the current position: Sell closes/reduces a long position, Buy closes/reduces
    // a short position.
    OrderSide side;
    Quantity quantity;
    std::optional<ClientOrderId> clientOrderId;
};
```

`CloseByMarketDraft` is a one-way-mode convenience for a normal `MARKET` order with `positionSide=BOTH` and `reduceOnly=true`. It is not the same as Binance conditional `closePosition=true`.

Hedge mode is intentionally not supported by `CloseByMarketDraft` in Phase 1 because Binance forbids sending `reduceOnly` in Hedge Mode. If the account is in Hedge Mode, `closeByMarket()` must fail before send with a validation error. A hedge-mode close must use an explicit `MarketOrderDraft` with `positionSide=Long` or `positionSide=Short`, no `reduceOnly`, and caller-owned position validation.

```cpp
// NormalOrderDraft is a variant over all concrete draft types accepted by batchNormal.
using NormalOrderDraft = std::variant<MarketOrderDraft, LimitOrderDraft, CloseByMarketDraft>;
```

`CloseByMarketDraft` is included in `NormalOrderDraft` because it maps to a normal one-way `MARKET reduceOnly=true` order and shares the normal order lifecycle. It is not an algo order and is rejected in Hedge Mode.

### 7.5 Algo Drafts (Phase 2 Contract)

Algo drafts are part of the design but are phase 2:

```cpp
struct StopMarketAlgoDraft {
    Symbol symbol;
    OrderSide side;
    TriggerPrice triggerPrice;
    PositionSide positionSide = PositionSide::Both;
    std::optional<Quantity> quantity;
    bool closePosition = false;
    WorkingType workingType = WorkingType::ContractPrice;
    std::optional<ClientAlgoId> clientAlgoId;
};
```

Important boundary:

- `STOP_MARKET`, `TAKE_PROFIT_MARKET`, `STOP`, `TAKE_PROFIT`, and `TRAILING_STOP_MARKET` use algo lifecycle in this design.
- `closePosition=true` belongs to algo conditional close-all, not `CloseByMarketDraft`.
- If implementation phase 1 does not support algo, these methods are unavailable or return explicit `Unsupported`.

### 7.6 Result Types

```cpp
// Returned by market(), limit(), closeByMarket(), and each item in BatchPlacementResult.
struct NormalPlacementResult {
    PlacementState state;
    Symbol symbol;
    ClientOrderId clientOrderId;
    CorrelationId correlationId;        // local journal correlation id for tracing
    std::optional<int64_t> orderId;     // present whenever Binance returns it
    std::optional<std::string> orderStatus; // status returned by Binance, not guaranteed final
    std::optional<OrderErrorCategory> errorCategory; // present when state is Rejected or Unknown
    std::optional<int> binanceCode;     // Binance API error code when state == Rejected
    std::optional<std::string> binanceMessage;
    std::optional<int> httpStatus;
    ValidationReport validation;        // always present; may have zero issues
};

// Returned by cancelNormalByOrderId() and cancelNormalByClientOrderId().
struct NormalCancelResult {
    Symbol symbol;
    int64_t orderId{0};
    ClientOrderId clientOrderId;
    std::string orderStatus;   // Binance order status string, e.g. "CANCELED"
    std::string side;          // raw string from Binance cancel response
    std::string type;
    std::string origQty;       // decimal string as returned by Binance
    std::string executedQty;
    std::string price;
};

// Returned by queryNormalByOrderId(), queryNormalByClientOrderId(), openNormalOrders(),
// and queryAllNormal(). Represents a point-in-time snapshot of a normal order.
struct NormalOrderSnapshot {
    Symbol symbol;
    int64_t orderId{0};
    ClientOrderId clientOrderId;
    OrderSide side{OrderSide::Buy};
    OrderType type{OrderType::Market};
    PositionSide positionSide{PositionSide::Both};
    TimeInForce timeInForce{TimeInForce::GTC};
    std::string status;        // raw Binance status: NEW, PARTIALLY_FILLED, FILLED, CANCELED, etc.
    std::string price;         // decimal string as returned by Binance
    std::string origQty;
    std::string executedQty;
    std::string avgPrice;
    std::string cumQuote;
    bool reduceOnly{false};
    bool closePosition{false};
    std::string stopPrice;
    WorkingType workingType{WorkingType::ContractPrice};
    int64_t time{0};           // order creation time, Unix ms
    int64_t updateTime{0};
};
```

Decimal fields in `NormalCancelResult` and `NormalOrderSnapshot` are `std::string` because they arrive from Binance and are already validated strings. `double` is not used to preserve precision, consistent with the outbound boundary rule in Section 8.

```cpp
struct BatchPlacementResult {
    std::vector<NormalPlacementResult> items; // same order as input drafts
};
```

### 7.7 OrdersConfig

```cpp
struct OrdersConfig {
    // Namespace embedded in generated client IDs. Max 8 ASCII chars from the
    // Binance client-id charset, preferably [A-Za-z0-9_].
    // Example: "strat01", "hedge_a". Must not be empty.
    std::string clientIdNamespace;

    // If false (default), placement fails unless a durable journal is configured.
    // Set true only for testing or strategies that accept reconcile loss on crash.
    bool allowBestEffortJournal = false;

    // Default response type for market() and limit(). Per-draft override takes precedence.
    ResponseType defaultResponseType = ResponseType::ACK;

    // recvWindow sent with signed requests. Keep conservative (≤5000ms recommended).
    std::chrono::milliseconds recvWindow = std::chrono::milliseconds{5000};

    // Allow raw params to set timestamp/recvWindow. False by default for safety.
    bool allowRawTimestampOverride = false;
};
```

---

## 8. Decimal Boundary

Do not expose `double` in order drafts for `price`, `quantity`, `triggerPrice`, `callbackRate`, or `activationPrice`.

```cpp
class DecimalString {
public:
    static Result<DecimalString> parse(std::string_view value);
    std::string_view value() const;

private:
    std::string m_value; // stored in canonical form after validation
};

using Price        = DecimalString;
using Quantity     = DecimalString;
using TriggerPrice = DecimalString;
```

Rules:

- Accepted format is ASCII decimal text: `1`, `1.0`, `0.001`, `12345.67`.
- Scientific notation, locale comma, whitespace, sign-only strings, and `NaN`/`inf` are rejected.
- The mapper preserves outbound decimal text after canonical validation.
- When symbol metadata is available, validator checks tick size, step size and min notional without silently rounding.
- Rounding/truncation is caller-owned unless a later explicit rounding policy object is added.

---

## 9. Validation Model

Validation has two classes:

| Class | Blocks placement | Examples |
|---|---:|---|
| Hard errors | Yes | missing symbol, invalid decimal, `LIMIT` without price, raw param conflicts, invalid client id |
| Warnings/skipped | No | stale exchange info, missing filter metadata, unknown margin state |

```cpp
struct ValidationIssue {
    enum class Severity { Error, Warning, Skipped };
    Severity severity;
    std::string code;
    std::string message;
};

struct ValidationReport {
    std::vector<ValidationIssue> issues;
    std::optional<std::chrono::milliseconds> exchangeInfoAge;
    bool hasErrors() const;
};
```

Only `Severity::Error` blocks client-side placement. Warnings and skipped checks are embedded in `NormalPlacementResult::validation` and returned to the caller on every placement call, whether the placement succeeded or failed. They never mean the order is guaranteed to pass server validation.

Hard validations:

- Required params per draft type.
- Mutual exclusion: `price` vs `priceMatch`, typed fields vs raw duplicates.
- `reduceOnly` cannot be used in contexts Binance forbids, when known.
- `CloseByMarketDraft` requires confirmed one-way mode; if account mode is Hedge or unavailable, placement fails before send.
- `closePosition=true` cannot be combined with `quantity` or `reduceOnly` in algo drafts.
- `clientOrderId`/`clientAlgoId` must satisfy Binance's documented id shape and length.
- `batchNormal` max size is 5.

Advisory validations:

- tick size, step size, min notional, symbol status.
- hedge/one-way position mode.
- trigger protection and immediate-trigger checks for algo orders.
- available margin, leverage, reduce-only conflicts.

---

## 10. Raw Params Guardrails

`RawOrderParams` exists only to avoid blocking newly added Binance parameters before typed support lands. It is not allowed to override the typed API.

Rules:

- Conflict between typed field and raw key is a validation error.
- Raw params cannot set `timestamp`, `signature`, `recvWindow` unless `OrdersConfig::allowRawTimestampOverride` is true.
- Raw params cannot set `symbol`, `side`, `type`, `quantity`, `price`, `newClientOrderId`, `clientAlgoId` if those are modeled by the draft.
- Raw param keys must match a conservative ASCII key pattern.
- Signed query strings, API keys, signatures and secrets are always redacted from logs.
- Raw params are forwarded exactly after validation; if Binance rejects them, the exchange error is returned with code/message/raw body.

---

## 11. OrderJournal Interface

`OrderJournal` is an abstract interface. `NormalOrderService` depends on it via reference. Two implementations exist:

- `InMemoryOrderJournal` — no crash durability; only valid with `allowBestEffortJournal = true`.
- `DurableOrderJournal` — wraps a persistent storage backend (open question: file, SQLite, or caller-provided adapter).

```cpp
struct JournalEntry {
    CorrelationId correlationId;
    Symbol symbol;
    ClientOrderId clientOrderId;
    // "normal" or "algo"
    std::string orderCategory;
    OrderSide side;
    OrderType type;
    PositionSide positionSide;
    // exact outbound decimal strings after canonicalization
    std::string quantity;
    std::string price;      // empty for MARKET
    // canonical request params, secret material excluded
    std::string requestParams;
    int64_t sendTimestampMs{0};
    std::optional<int64_t> responseTimestampMs;
    PlacementState state{PlacementState::UnknownPendingReconcile};
    std::optional<int64_t> binanceOrderId;
};

class OrderJournal {
public:
    virtual ~OrderJournal() = default;

    // Persist the intent before the request is sent.
    // Must complete before IRestClient::newOrder is called.
    // Returns error if persistence fails; caller must not send if this fails
    // (unless allowBestEffortJournal is true, in which case InMemoryOrderJournal is used).
    virtual Result<void> recordIntent(JournalEntry entry) = 0;

    // Update state after response or on timeout/cancellation.
    virtual Result<void> updateState(CorrelationId id,
                                     PlacementState state,
                                     std::optional<int64_t> binanceOrderId = std::nullopt) = 0;

    // Returns all entries whose state is UnknownPendingReconcile.
    // Used at startup and by the reconcile loop.
    virtual Result<std::vector<JournalEntry>> pendingReconcile() = 0;

    // Look up a journal entry by client order id.
    // Used during reconcile to retrieve the original intent.
    virtual Result<std::optional<JournalEntry>> findByClientOrderId(
        ClientOrderId clientOrderId) = 0;
};
```

---

## 12. Placement Lifecycle

### 12.1 Normal Placement Flow

```
draft
  │
  ├─ generate clientOrderId if missing
  ├─ validate hard rules → return error immediately if any
  ├─ collect advisory warnings/skipped checks
  ├─ persist intent in OrderJournal  ← must succeed before send
  ├─ map to REST params
  ├─ POST /fapi/v1/order or /fapi/v1/batchOrders
  ├─ map response/error
  └─ update journal with Accepted / Rejected / UnknownPendingReconcile
```

### 12.2 Placement State

```cpp
enum class PlacementState {
    Accepted,
    Rejected,
    UnknownPendingReconcile
};
```

`UnknownPendingReconcile` means the server outcome is unknown. The order may already exist, may already be filled, or may not exist. Caller must not blindly send another create order for the same trading intent.

Return `UnknownPendingReconcile` for:

- timeout after the request may have been written
- network disconnect after write
- coroutine cancellation after write
- parse failure or unexpected response body after a successful write
- selected 5xx or transport errors where commit cannot be ruled out

Return client-side error before send for:

- validation hard error
- journal persistence failure when durable journal is required
- coroutine cancellation before the request is written

### 12.3 Response Type

Default `newOrderRespType` is `ACK`, configured via `OrdersConfig::defaultResponseType`. Per-draft `responseType` field overrides the config default.

`RESULT` is opt-in only because it can increase latency and ambiguity on timeout. Even when `RESULT` is used, timeout after send still maps to `UnknownPendingReconcile`. With `ACK`, Binance may still return `orderId` and current `status`; the SDK must preserve those fields when present, but caller must not treat them as final fill state.

---

## 13. Reconcile

Reconcile uses client id as the primary identity:

- normal: `queryNormalByClientOrderId(symbol, clientOrderId)` maps to `/fapi/v1/order?symbol=...&origClientOrderId=...`
- algo: `queryAlgoByClientAlgoId(clientAlgoId)` maps to `/fapi/v1/algoOrder?clientAlgoId=...`

Constraints:

- Generate client ids before journal write.
- Persist journal before sending the request when durable mode is enabled.
- If no durable journal is configured, placement is rejected unless caller explicitly sets `allowBestEffortJournal=true`.
- Best-effort journal only supports reconcile while the process is alive.
- Query retention is limited by Binance; canceled/expired no-fill orders can disappear after 3 days and orders older than 90 days can disappear. The journal must keep local intent even when Binance no longer returns the order.
- `queryAllNormal` requests must use a time period shorter than 7 days. Reconcile over a wider range must split the scan into smaller windows.
- Reconcile polling must use backoff with jitter and respect rate limits.
- When reconcile finds the order via `queryAllNormal`, the journal entry must be updated and the caller notified.

---

## 14. Client ID Generation

The SDK generates ids by default:

```
btb_<namespace>_<timestamp_ms>_<entropy>
```

### 14.1 Length Budget

Binance documented limit: 1–36 characters. Allowed charset: `[a-zA-Z0-9._:@/-]`.

| Component | Length | Example |
|---|---|---|
| `btb_` prefix | 4 | `btb_` |
| namespace | ≤8 | `strat01` |
| `_` separator | 1 | `_` |
| timestamp ms (13 digits through 2286) | 13 | `1747044000000` |
| `_` separator | 1 | `_` |
| entropy (8 hex chars = 32-bit random) | 8 | `a3f92c10` |
| **Total** | **≤35** | fits within 36 |

`OrdersConfig::clientIdNamespace` must be ≤8 characters. `OrderIdGenerator` rejects namespaces that would cause the full id to exceed 36 characters.

### 14.2 Collision Avoidance

- Timestamp component is Unix milliseconds at generation time.
- Entropy component is a cryptographically random 32-bit value, rendered as 8 lowercase hex digits.
- Timestamp + 32-bit entropy gives ~4 billion unique ids per millisecond per namespace. Collision probability within a single millisecond is negligible for trading workloads.
- Across process restarts: the timestamp advances monotonically, so restarts do not collide with prior runs as long as the clock is not set backwards.
- Across parallel processes: namespace must differ between concurrent processes (e.g., `strat01` vs `strat02`). If the same namespace is used across processes, random entropy is the sole collision guard.

### 14.3 Intent Retry

- A retry of the same trading intent must reuse the same journaled client id.
- A new trading intent must get a new client id.
- Caller override is allowed only if it passes validation.

---

## 15. Batch Normal Orders

`batchNormal` maps to `/fapi/v1/batchOrders`.

Rules:

- Max 5 orders per call. Validation rejects `drafts.size() > 5` before any network call.
- `NormalOrderDraft` is `std::variant<MarketOrderDraft, LimitOrderDraft, CloseByMarketDraft>`. The mapper visits each variant to produce the per-item params.
- Parameter rules follow normal order rules.
- Binance processes batch orders concurrently; matching order is not guaranteed.
- Returned response order follows input order, but each item may succeed or fail independently.
- No all-or-nothing semantics.
- Do not use batch for dependent sequencing like "open only if previous close succeeds".
- Batch result is per item; each item can be `Accepted`, `Rejected`, or `UnknownPendingReconcile`.

```cpp
struct BatchPlacementResult {
    std::vector<NormalPlacementResult> items; // same order as input drafts
};
```

---

## 16. Error Taxonomy

`BinanceError` can remain the low-level transport/API error. Orders layer adds order-aware classification:

```cpp
enum class OrderErrorCategory {
    Validation,
    Unsupported,
    ExchangeReject,
    RateLimit,
    Auth,
    Network,
    Timeout,
    CanceledBeforeSend,
    Parse,
    Journal,
    Unknown
};
```

Placement result keeps:

- optional error category, present for rejected and unknown outcomes
- Binance code/message when present
- HTTP status when present
- endpoint
- local correlation id
- client id
- raw response body with sensitive fields redacted

Do not parse error strings in strategy code; use category and Binance code.

---

## 17. Rate Limits And Time Sync

Orders layer must cooperate with `RateLimiter`, but `RateLimiter` needs to understand order-specific limits:

- `/fapi/v1/order` charges order-count headers, not IP weight.
- `/fapi/v1/batchOrders` charges batch-specific order-count and IP weight.
- query/cancel endpoints have their own weights.
- response headers such as `X-MBX-ORDER-COUNT-10S`, `X-MBX-ORDER-COUNT-1M`, and `x-mbx-used-weight-1m` must update limiter state.
- reconcile polling must have backoff and jitter.

Signed endpoints require reliable timestamp handling:

- Maintain server-time offset from `/fapi/v1/time`.
- Generate monotonic signed timestamps.
- Surface clock drift as `Auth` or `TimeSync`-style error before repeated order failures.
- Keep default `recvWindow` conservative; allow config via `OrdersConfig::recvWindow` but avoid broad defaults.

---

## 18. Security And Observability

Logging/metrics must include enough data for operations without leaking secrets:

- correlation id
- client order/algo id
- symbol
- endpoint
- latency
- placement state
- error category and Binance code
- rate-limit header values
- count of `UnknownPendingReconcile`

Never log:

- API key
- secret key
- signature
- full signed query string
- account balances or positions unless a caller explicitly logs them outside this layer

---

## 19. Examples

### 19.1 Market Buy

```cpp
MarketOrderDraft draft{
    .symbol = Symbol{"BTCUSDT"},
    .side = OrderSide::Buy,
    .quantity = Quantity::parse("0.001").value(),
};

auto result = co_await orders.market(std::move(draft));
if (!result) {
    // client-side validation, journal, auth, transport setup, etc.
    co_return;
}

switch (result->state) {
    case PlacementState::Accepted:
        // ACK means accepted by Binance, not necessarily filled.
        // Preserve orderId/status if Binance returned them, but query or stream for final fills.
        break;
    case PlacementState::Rejected:
        // Exchange rejected or client-side mapped rejection.
        break;
    case PlacementState::UnknownPendingReconcile:
        // Do not blindly retry create. Query by result->clientOrderId.
        break;
}
```

### 19.2 Limit Sell

```cpp
LimitOrderDraft draft{
    .symbol = Symbol{"ETHUSDT"},
    .side = OrderSide::Sell,
    .quantity = Quantity::parse("0.05").value(),
    .price = Price::parse("3500.00").value(),
    .timeInForce = TimeInForce::GTC,
};

auto result = co_await orders.limit(std::move(draft));
```

### 19.3 Reconcile Unknown

```cpp
if (placement.state == PlacementState::UnknownPendingReconcile) {
    auto snapshot = co_await orders.queryNormalByClientOrderId(
        placement.symbol,
        placement.clientOrderId);

    if (!snapshot) {
        // Not found in open orders; try order history.
        auto history = co_await orders.queryAllNormal(placement.symbol);
        // Continue scheduled reconcile with backoff unless retention window is exceeded.
    }
}
```

### 19.4 Batch Mixed Result

```cpp
std::vector<NormalOrderDraft> drafts;
drafts.push_back(MarketOrderDraft{ /* ... */ });
drafts.push_back(LimitOrderDraft{ /* ... */ });

auto batch = co_await orders.batchNormal(std::move(drafts));
if (batch) {
    for (const auto& item : batch->items) {
        // Handle each item independently.
    }
}
```

### 19.5 Emergency Cancel All

```cpp
// Cancel all open normal orders for a symbol on emergency shutdown.
auto result = co_await orders.cancelAllNormal(Symbol{"BTCUSDT"});
if (!result) {
    // Log and alert; manual intervention may be required.
}
```

---

## 20. Testing Strategy

Unit tests:

- Decimal parsing and canonical rejection.
- Client id format, length budget, and collision-resistant generation boundaries.
- Draft validation for market, limit, close-by-market.
- `closeByMarket` rejects Hedge Mode and rejects unknown position mode before send.
- Raw param conflict detection.
- Mapping from drafts to exact Binance params (`std::visit` over `NormalOrderDraft` variant).
- Placement state mapping for accepted, rejected, timeout after send, cancellation before/after send.
- Batch per-item response mapping.
- `ValidationReport` is embedded in `NormalPlacementResult` on both success and failure paths.

Integration-style tests with `FakeRestClient` (implements `IRestClient`):

- Journal write occurs before send.
- Timeout after send returns `UnknownPendingReconcile`.
- Reconcile queries by client id via `queryNormalByClientOrderId`, then `queryAllNormal`.
- `queryAllNormal` rejects or chunks request windows that are 7 days or longer.
- Durable journal failure blocks placement unless best-effort mode is explicit.
- Rate-limit headers are passed into limiter.
- `cancelAllNormal` invokes the correct endpoint with symbol param.
- `queryAllNormal` passes through time range and limit params.

No test should hit production Binance by default.

---

## 21. Phased Delivery

### Phase 1 — Normal Orders

- `Orders` facade for market, limit, close-by-market.
- Normal cancel/query/open/all + cancel-all.
- Batch normal with max 5 and per-item result.
- Decimal boundary.
- Client id generation with length budget and entropy spec.
- Validation report embedded in placement result.
- Journal interface with in-memory and durable adapter boundary.
- Unknown/reconcile semantics.
- `IRestClient` interface extracted for test injection.

### Phase 2 — Algo Orders

- `AlgoOrderService`.
- Stop market, take profit market, stop limit, take profit limit, trailing stop market.
- Algo query/cancel/open.
- `clientAlgoId` generation and journal integration.
- Trigger/close-position validation rules.

### Phase 3 — Legacy Migration

- Migrate `BinanceAPI::createOrder` to use `Orders`.
- Deprecate stringly typed side/type placement.
- Add compatibility wrapper only where needed.

---

## 22. Decision Log

| Decision | Alternatives Considered | Objections Raised | Resolution |
|---|---|---|---|
| Add `orders` layer above `RestClient` | Keep only generic `OrderRequest` | Generic request is easy to misuse for strategy code | Accepted; typed layer reduces mistakes but raw escape hatch remains guarded |
| Split normal and algo lifecycle | Single `OrderService` for all order types | Algo endpoints have different response/query/cancel semantics | Accepted; normal and algo services are separate |
| Phase 1 focuses on normal market/limit | Implement all Binance order types immediately | Larger surface increases risk and false confidence | Accepted; algo has design contract but can be phase 2 |
| Use decimal strings at order boundary | Continue using `double` | Precision/rounding can create reject or wrong order values | Accepted; no silent rounding |
| Auto-generate client ids | Rely on Binance auto id or caller-provided id | Reconcile after timeout requires known id | Accepted; SDK generates by default and journals before send |
| Add `UnknownPendingReconcile` | Return network error as rejected/failed | Timeout after send can mean order exists | Accepted; unknown is first-class and must not be retried blindly |
| Require journal for production durability | In-memory only | Crash after send can lose client id/intent | Accepted; durable journal required unless caller explicit best-effort |
| Default placement response type is `ACK` | Default `RESULT` for more data | `RESULT` increases latency and ambiguity on timeout | Accepted; `RESULT` is opt-in via `OrdersConfig` or per-draft field |
| Validator is advisory for dynamic checks | Try to guarantee client-side validity | Metadata/account state can be stale | Accepted; hard errors block, warnings/skipped are surfaced in `NormalPlacementResult::validation` |
| Raw params are allowed with guardrails | No escape hatch, or raw overrides typed fields | No escape hatch blocks Binance changes; unrestricted raw breaks invariants | Accepted; raw conflicts are validation errors |
| Batch returns per-item results | All-or-nothing result | Binance returns mixed success/failure and concurrent matching | Accepted; no atomicity assumption |
| `NormalOrderDraft` is `std::variant` | Polymorphic base class or tagged union | `std::variant` is idiomatic C++17/20, avoids heap allocation, exhaustive `std::visit` | Accepted; mapper uses `std::visit` to handle each concrete type |
| `CloseByMarketDraft` is one-way-mode only | Support hedge-mode close with `reduceOnly=true` | Binance forbids `reduceOnly` in Hedge Mode, so this would be rejected or unsafe | Accepted; Phase 1 rejects hedge-mode close-by-market before send |
| Add `cancelAllNormal` and `queryAllNormal` to facade | Leave out of v1 | Emergency shutdown needs cancel-all; reconcile needs history query beyond open orders | Accepted; both added to Phase 1 facade |
| `ValidationReport` embedded in `NormalPlacementResult` | Return separately from placement result | Separate return complicates call sites; warnings are always contextual to a placement | Accepted; always present in result, may be empty |
| `OrderJournal` is abstract interface | Concrete class with compile-time switch | Interface enables `FakeOrderJournal` in tests without linking storage backend | Accepted; `IRestClient` follows same pattern |
| Client id entropy: timestamp_ms + random 32-bit | UUID v4 / sequential counter only | UUID (128-bit) exceeds 36-char budget as hex; sequential counter alone collides across restarts | Accepted; 13-char timestamp + 8-char random hex fits budget and avoids restart collisions |
| `IRestClient` extracted as interface | Test `NormalOrderService` via `RestClient` directly | Direct `RestClient` dependency requires network in tests | Accepted; `IRestClient` is injected; `FakeRestClient` used in unit/integration tests |

---

## 23. Review And Arbitration

### Skeptic / Challenger

Accepted objections:

- Normal and algo orders must not share a fake lifecycle.
- Placement result needs `UnknownPendingReconcile`.
- `clientOrderId` must be generated by default.
- Validator must not imply guaranteed server accept.
- Batch must be per-item and non-atomic.
- Close-position naming must distinguish market reduce-only close from conditional close-all.
- `NormalOrderDraft` must be a concrete variant type, not an undefined alias.
- Result types (`NormalPlacementResult`, `NormalCancelResult`, `NormalOrderSnapshot`) must be fully specified before implementation begins.

Rejected objections:

- "Do not add typed requests because they can drift" is rejected as a blocker. The design keeps a guarded raw escape hatch and explicit phase boundaries to manage drift.

### Constraint Guardian

Accepted objections:

- Durable reconcile requires journal before send.
- Rate limiter must model order-count and IP weight separately.
- Raw params require security guardrails.
- Cancellation after write maps to unknown.
- `ACK` is the default.
- Time sync/recvWindow handling is a reliability requirement.
- Observability must be present without logging secrets.
- `openNormalOrders` without symbol has request weight 40; callers should prefer symbol-scoped calls.
- `cancelAllNormal` and `queryAllNormal` are required for emergency shutdown and post-crash reconcile respectively.
- `queryAllNormal` must respect Binance's less-than-7-day query window.

No constraint objections were rejected.

### User Advocate

Accepted objections:

- Unknown state needs explicit docs and examples.
- Normal vs algo error messages must be clear.
- Validation report severity must be unambiguous.
- Decimal accepted format must be documented.
- Journal tradeoff must be visible.
- Batch warnings must appear near API docs.
- `CloseByMarketDraft` must not imply hedge-mode support because Binance forbids `reduceOnly` in Hedge Mode.

Clarifications resolved:

- `UnknownPendingReconcile` applies to any post-write ambiguous outcome.
- Without durable journal, placement fails unless best-effort mode is explicitly enabled via `OrdersConfig`.
- Raw param conflicts are validation errors.
- Only validation errors block client-side placement.
- Algo API is compile-time unavailable or explicit runtime `Unsupported`; never silently normal-routed.
- `cancelNormalByClientOrderId` maps to `origClientOrderId` at the REST level; these names are synonymous.
- `ValidationReport` is always embedded in `NormalPlacementResult`; callers do not need a separate call to retrieve warnings.
- `ACK` responses may include `orderId`/`status`; the SDK preserves them when returned, but callers still query or stream for final fill state.

### Arbiter Decision

The design is acceptable for implementation planning. All high-severity objections were accepted and incorporated. Remaining open questions (storage backend for durable journal, legacy `createOrder` deprecation timeline) are implementation choices, not blockers for the architecture.

**Final disposition:** APPROVED.
