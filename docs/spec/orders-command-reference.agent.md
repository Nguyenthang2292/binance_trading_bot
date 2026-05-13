---
doc_type: agent-reference
source: docs/spec/orders-command-reference.md
generated: 2026-05-13
target_audience: AI coding agents
language: en
format_notes: >
  Structured for deterministic parsing. All types are C++20 exact.
  Use "source_file" fields to navigate the codebase.
  "INVARIANT" lines are hard constraints — violating them is a bug.
  "PRECONDITION" lines must be true before calling.
  "POSTCONDITION" lines are guaranteed on success path.
---

# Orders Layer — Agent Reference

## 0. Quick Navigation

| Task | Section |
|---|---|
| Find a method signature | §2 Normal API, §3 Algo API |
| Understand a type | §5 Type Catalog |
| Know what fails validation | §4 Validation Rules |
| Map MQL4 operations | §6 MQL4 Adapter |
| Understand snapshot/fill APIs | §2.C Snapshot APIs |
| Understand journal behavior | §7 Journal Rules |
| Find source file for a symbol | §8 File Index |

---

## 1. Architecture

```
Orders                          ← thin facade, delegates only
├── NormalOrderService          ← market / limit / amend / cancel / query / batch
└── AlgoOrderService            ← stopEntry / protection / algo cancel / algo query

orders::mql4::Mql4Adapter       ← optional MQL4 compatibility wrapper over Orders
```

- `Orders` holds `unique_ptr<NormalOrderService>` + `unique_ptr<AlgoOrderService>`.
- Every public `Orders` method is a 1-line `co_return co_await m_*Service->...`.
- Business logic lives exclusively in the two service classes.
- INVARIANT: `Orders` contains zero business logic.

---

## 2. Normal Order API

Return type for all methods: `boost::asio::awaitable<OrdersResult<T>>`  
where `OrdersResult<T> = std::expected<T, BinanceError>`.

### 2.A Placement

#### `market`
```
Orders::market(MarketOrderDraft draft)
  → OrdersResult<NormalPlacementResult>
```
- REST: `POST /fapi/v1/order` with `type=MARKET`
- PRECONDITION: `draft.symbol` non-empty, `draft.quantity > 0`
- PRECONDITION: `draft.reduceOnly` must be `nullopt` or `false` when `positionMode == Hedge`
- POSTCONDITION: result always contains `validation` (even on `Rejected`)
- POSTCONDITION: on network/parse ambiguity → `state = UnknownPendingReconcile`
- POSTCONDITION: journal entry written before REST send when journal configured
- ERROR → `OrderErrorCategory::Validation` when preconditions fail

#### `limit`
```
Orders::limit(LimitOrderDraft draft)
  → OrdersResult<NormalPlacementResult>
```
- REST: `POST /fapi/v1/order` with `type=LIMIT`
- PRECONDITION: all of `market` preconditions
- PRECONDITION: `draft.price > 0`
- Same postconditions as `market`

#### `closeByMarket`
```
Orders::closeByMarket(CloseByMarketDraft draft)
  → OrdersResult<NormalPlacementResult>
```
- REST: `POST /fapi/v1/order` with `reduceOnly=true`
- PRECONDITION: `positionMode == PositionMode::OneWay`
- ERROR → `Validation` if `positionMode != OneWay`

#### `batchNormal`
```
Orders::batchNormal(std::vector<NormalOrderDraft> drafts)
  → OrdersResult<BatchPlacementResult>
```
- REST: `POST /fapi/v1/batchOrders`
- INVARIANT: `drafts.size() <= 5`
- POSTCONDITION: `BatchPlacementResult::items.size() == drafts.size()`
- POSTCONDITION: each item is an independent `NormalPlacementResult`
- ERROR → all items rejected if batch-level validation fails

### 2.B Amendment

#### `amendLimitOrder` (canonical)
```
Orders::amendLimitOrder(AmendLimitOrderDraft draft)
  → OrdersResult<NormalOrderSnapshot>
```
- REST: `PUT /fapi/v1/order`
- PRECONDITION: target order must be a LIMIT order on Binance
- PRECONDITION: both `quantity` and `price` required
- WARNING: amending reorders queue priority
- WARNING: invalid quantity reduction or GTX immediate execution can cancel the order
- Ambiguous outcome → reconcile by querying identity

#### `amendLimitOrderByOrderId` (convenience)
```
Orders::amendLimitOrderByOrderId(
    Symbol symbol,
    OrderSide side,
    int64_t orderId,
    Quantity quantity,
    Price price,
    std::optional<ResponseType> responseType = nullopt,
    std::optional<int64_t> recvWindow = nullopt)
  → OrdersResult<NormalOrderSnapshot>
```
- Constructs `AmendLimitOrderDraft` and delegates to canonical path
- INVARIANT: `responseType` and `recvWindow` are forwarded, never dropped

#### `amendLimitOrderByClientOrderId` (convenience)
```
Orders::amendLimitOrderByClientOrderId(
    Symbol symbol,
    OrderSide side,
    ClientOrderId clientOrderId,
    Quantity quantity,
    Price price,
    std::optional<ResponseType> responseType = nullopt,
    std::optional<int64_t> recvWindow = nullopt)
  → OrdersResult<NormalOrderSnapshot>
```
- Same rules as `amendLimitOrderByOrderId`

### 2.C Cancel

#### `cancelNormalByOrderId`
```
Orders::cancelNormalByOrderId(Symbol symbol, int64_t orderId)
  → OrdersResult<NormalCancelResult>
```
- REST: `DELETE /fapi/v1/order?symbol=...&orderId=...`
- INVARIANT: `symbol` always required alongside `orderId`

#### `cancelNormalByClientOrderId`
```
Orders::cancelNormalByClientOrderId(Symbol symbol, ClientOrderId clientOrderId)
  → OrdersResult<NormalCancelResult>
```
- REST: `DELETE /fapi/v1/order?symbol=...&origClientOrderId=...`

#### `cancelAllNormal`
```
Orders::cancelAllNormal(Symbol symbol)
  → OrdersResult<void>
```
- REST: `DELETE /fapi/v1/allOpenOrders?symbol=...`

### 2.D Query

#### `queryNormalByOrderId`
```
Orders::queryNormalByOrderId(Symbol symbol, int64_t orderId)
  → OrdersResult<NormalOrderSnapshot>
```
- REST: `GET /fapi/v1/order?symbol=...&orderId=...`

#### `queryNormalByClientOrderId`
```
Orders::queryNormalByClientOrderId(Symbol symbol, ClientOrderId clientOrderId)
  → OrdersResult<NormalOrderSnapshot>
```
- REST: `GET /fapi/v1/order?symbol=...&origClientOrderId=...`

#### `openNormalOrders`
```
Orders::openNormalOrders(std::optional<Symbol> symbol = nullopt)
  → OrdersResult<std::vector<NormalOrderSnapshot>>
```
- REST: `GET /fapi/v1/openOrders`
- Omit symbol → all symbols (higher weight cost)

#### `queryAllNormal`
```
Orders::queryAllNormal(
    Symbol symbol,
    std::optional<int64_t> startTime = nullopt,
    std::optional<int64_t> endTime   = nullopt,
    int limit = 500)
  → OrdersResult<std::vector<NormalOrderSnapshot>>
```
- REST: `GET /fapi/v1/allOrders`
- INVARIANT: when both `startTime` and `endTime` provided, window must be `< 7 days`
- ERROR `-90007` if window `>= 7 days`

### 2.E Snapshot APIs (with Journal Metadata)

#### `openNormalOrderSnapshot`
```
Orders::openNormalOrderSnapshot(std::optional<Symbol> symbol = nullopt)
  → OrdersResult<OrderPoolSnapshot>
```
- Calls `openNormalOrders` then enriches each `NormalOrderSnapshot` with journal metadata
- POSTCONDITION: `snapshot.kind == OrderPoolKind::Open`
- POSTCONDITION: `snapshot.capturedAt` is set to `system_clock::now()` at construction
- POSTCONDITION: `OrderView::metadata` populated if `clientOrderId` found in journal
- INVARIANT: snapshot is immutable after return; does not auto-refresh

#### `queryAllNormalSnapshot`
```
Orders::queryAllNormalSnapshot(
    Symbol symbol,
    std::optional<int64_t> startTime = nullopt,
    std::optional<int64_t> endTime   = nullopt,
    int limit = 500)
  → OrdersResult<OrderPoolSnapshot>
```
- Same enrichment as `openNormalOrderSnapshot`
- POSTCONDITION: `snapshot.kind == OrderPoolKind::History`
- Inherits 7-day window invariant from `queryAllNormal`

#### `queryOrderFillSummary`
```
Orders::queryOrderFillSummary(Symbol symbol, int64_t orderId)
  → OrdersResult<OrderFillSummary>
```
- REST: `GET /fapi/v1/userTrades?symbol=...&orderId=...`
- POSTCONDITION: `completeness == Unavailable` when `userTrades` returns empty
- POSTCONDITION: `completeness == Partial` when any trade line fails to parse
- POSTCONDITION: `completeness == Complete` when all trades parsed successfully
- INVARIANT: monetary fields (`avgEntryPrice`, `realizedPnl`, `commission`) are
  `std::optional<std::string>` — absent means no data, not zero

---

## 3. Algo Order API

### 3.A Placement

#### `stopEntry`
```
Orders::stopEntry(StopEntryDraft draft)
  → OrdersResult<NormalPlacementResult>
```
- REST call path currently reuses `IRestClient::newOrder` (`POST /fapi/v1/order`)
- Result metadata endpoint tag is set to `/fapi/v1/algoOrder`
- `draft.limitPrice` absent → `STOP_MARKET`
- `draft.limitPrice` present → `STOP` (stop-limit)
- POSTCONDITION: `result.orderId` is the `orderId` parsed from order response payload
- POSTCONDITION: `clientAlgoId` is mapped onto `newClientOrderId` and can be queried via `queryAlgoByClientAlgoId`

#### `protection`
```
Orders::protection(ProtectionOrderDraft draft)
  → OrdersResult<NormalPlacementResult>
```
- `draft.kind == ProtectionKind::StopLoss` → `STOP_MARKET`
- `draft.kind == ProtectionKind::TakeProfit` → `TAKE_PROFIT_MARKET`
- `draft.closeQuantity = Quantity{...}` → explicit quantity on Binance
- `draft.closeQuantity = CloseEntirePosition{}` → `closePosition=true` on Binance
- INVARIANT: cannot pass both `Quantity` and `closePosition=true`; enforced at type level

### 3.B Cancel

#### `cancelAlgoByAlgoId`
```
Orders::cancelAlgoByAlgoId(Symbol symbol, int64_t algoId)
  → OrdersResult<NormalCancelResult>
```
- Implementation delegates to `IRestClient::cancelOrder(symbol, algoId)` (normal-order endpoint shape)

#### `cancelAlgoByClientAlgoId`
```
Orders::cancelAlgoByClientAlgoId(Symbol symbol, ClientAlgoId clientAlgoId)
  → OrdersResult<NormalCancelResult>
```
- Implementation delegates to `IRestClient::cancelOrderByClientOrderId(symbol, clientAlgoId)`

### 3.C Query

#### `queryAlgoByAlgoId`
```
Orders::queryAlgoByAlgoId(Symbol symbol, int64_t algoId)
  → OrdersResult<NormalOrderSnapshot>
```
- Implementation delegates to `IRestClient::queryOrder(symbol, algoId)` (normal-order endpoint shape)

#### `queryAlgoByClientAlgoId`
```
Orders::queryAlgoByClientAlgoId(Symbol symbol, ClientAlgoId clientAlgoId)
  → OrdersResult<NormalOrderSnapshot>
```
- Implementation delegates to `IRestClient::queryOrderByClientOrderId(symbol, clientAlgoId)`

---

## 4. Validation Rules

Source: `src/orders/order_validator.cpp`

### 4.A Hard Errors (block placement)

| Rule | Condition | Error code |
|---|---|---|
| symbol required | `draft.symbol.empty()` | `symbol_required` |
| quantity required | `draft.quantity.value().empty()` | `quantity_required` |
| quantity positive | `draft.quantity.toDouble() <= 0.0` | `quantity_positive` |
| limit price required | `draft.price.value().empty()` (limit only) | `limit_price_required` |
| limit price positive | `draft.price.toDouble() <= 0.0` (limit only) | `limit_price_positive` |
| clientOrderId too long | `clientOrderId->size() > 36` | `client_order_id_too_long` |
| reduceOnly in Hedge | `reduceOnly=true && positionMode==Hedge` | `reduce_only_hedge_forbidden` |
| closeByMarket mode | `positionMode != OneWay` | `close_by_market_requires_one_way` |
| trigger price required | `draft.triggerPrice.value().empty()` (stop/protection) | `trigger_price_required` |
| trigger price positive | `draft.triggerPrice.toDouble() <= 0.0` (stop/protection) | `trigger_price_positive` |
| stop-entry limitPrice empty | `draft.limitPrice && draft.limitPrice->value().empty()` | `limit_price_empty` |
| stop-entry limitPrice positive | `draft.limitPrice && draft.limitPrice->toDouble() <= 0.0` | `limit_price_positive` |
| batch empty | `drafts.size() == 0` | `batch_empty` |
| batch too large | `drafts.size() > 5` | `batch_too_large` |
| raw key empty | `key.empty()` | `raw_param_key_empty` |
| raw key format | fails `^[A-Za-z][A-Za-z0-9_]{0,63}$` | `raw_param_key_invalid` |
| raw blocked key | key in blocked set (see §4.B) | `raw_param_blocked` |
| raw timestamp override | `(recvWindow\|timestamp\|signature) && !allowRawTimestampOverride` | `raw_recvwindow_blocked` |

### 4.B Raw Blocked Keys (always blocked)

```
symbol, side, type, quantity, price, positionSide,
newClientOrderId, newOrderRespType, timeInForce,
reduceOnly, clientAlgoId
```

Conditionally blocked (when `allowRawTimestampOverride=false`):
```
recvWindow, timestamp, signature
```

### 4.C Advisory Issues (never block placement)

| Severity | Code | Condition |
|---|---|---|
| `Warning` | `position_mode_unknown` | `positionMode == Unknown` |
| `Warning` | `no_client_id_namespace` | `clientIdNamespace.empty()` |
| `Skipped` | `exchange_info_unavailable` | always (no exchange-info snapshot) |

INVARIANT: `NormalPlacementResult::validation` is always present, on both success and failure paths.

---

## 5. Type Catalog

### 5.A Input Drafts

```cpp
// source: src/orders/order_drafts.h

struct MarketOrderDraft {
    Symbol symbol;
    OrderSide side{Buy};
    Quantity quantity;                        // DecimalString, must be > 0
    PositionSide positionSide{Both};
    std::optional<bool> reduceOnly;
    std::optional<ClientOrderId> clientOrderId;
    std::optional<ResponseType> responseType;
    RawOrderParams raw;                       // unordered_map<string,string>
    std::optional<OrderMetadata> metadata;
};

struct LimitOrderDraft {
    Symbol symbol;
    OrderSide side{Buy};
    Quantity quantity;
    Price price;                              // DecimalString, must be > 0
    TimeInForce timeInForce{GTC};
    PositionSide positionSide{Both};
    std::optional<bool> reduceOnly;
    std::optional<ClientOrderId> clientOrderId;
    std::optional<ResponseType> responseType;
    RawOrderParams raw;
    std::optional<OrderMetadata> metadata;
};

struct CloseByMarketDraft {
    Symbol symbol;
    OrderSide side;
    Quantity quantity;
    std::optional<ClientOrderId> clientOrderId;
    std::optional<OrderMetadata> metadata;
};

struct AmendLimitOrderDraft {
    NormalOrderIdentity identity;
    OrderSide side;                           // required by Binance PUT endpoint
    Quantity quantity;
    Price price;
    std::optional<ResponseType> responseType;
    std::optional<int64_t> recvWindow;
};

struct StopEntryDraft {
    Symbol symbol;
    OrderSide side;
    Quantity quantity;
    TriggerPrice triggerPrice;
    std::optional<Price> limitPrice;          // absent → STOP_MARKET; present → STOP (limit)
    WorkingType workingType{ContractPrice};
    std::optional<ClientAlgoId> clientAlgoId;
    std::optional<OrderMetadata> metadata;
};

struct ProtectionOrderDraft {
    Symbol symbol;
    PositionSide positionSide;
    OrderSide closeSide;
    ProtectionKind kind{StopLoss};
    TriggerPrice triggerPrice;
    std::variant<Quantity, CloseEntirePosition> closeQuantity;  // type-enforced mutual exclusion
    std::optional<ClientAlgoId> clientAlgoId;
    std::optional<OrderMetadata> metadata;
};

using NormalOrderDraft = std::variant<MarketOrderDraft, LimitOrderDraft, CloseByMarketDraft>;
```

### 5.B Result Types

```cpp
// source: src/orders/order_result.h

struct NormalPlacementResult {
    PlacementState state;                         // Accepted | Rejected | UnknownPendingReconcile
    Symbol symbol;
    ClientOrderId clientOrderId;
    CorrelationId correlationId;
    std::optional<int64_t> orderId;
    std::optional<std::string> orderStatus;
    std::optional<OrderErrorCategory> errorCategory;
    std::optional<int> binanceCode;
    std::optional<std::string> binanceMessage;
    std::optional<int> httpStatus;
    std::optional<std::string> endpoint;
    std::optional<std::string> rawResponseBody;
    ValidationReport validation;                  // ALWAYS present
};

struct NormalCancelResult {
    Symbol symbol;
    int64_t orderId;
    ClientOrderId clientOrderId;
    std::string orderStatus;
    std::string side;
    std::string type;
    std::string origQty;        // decimal string
    std::string executedQty;    // decimal string
    std::string price;          // decimal string
};

struct NormalOrderSnapshot {
    Symbol symbol;
    int64_t orderId;
    ClientOrderId clientOrderId;
    OrderSide side;
    OrderType type;
    PositionSide positionSide;
    TimeInForce timeInForce;
    std::string status;
    std::string price;          // decimal string
    std::string origQty;        // decimal string
    std::string executedQty;    // decimal string
    std::string avgPrice;       // decimal string
    std::string cumQuote;       // decimal string
    bool reduceOnly;
    bool closePosition;
    std::string stopPrice;      // decimal string
    WorkingType workingType;
    int64_t time;
    int64_t updateTime;
};

struct BatchPlacementResult {
    std::vector<NormalPlacementResult> items;
};

struct OrderFillSummary {
    Symbol symbol;
    int64_t orderId;
    FillSummaryCompleteness completeness;   // Unavailable | Partial | Complete
    std::string executedQty;               // decimal string
    std::optional<std::string> avgEntryPrice;
    std::optional<std::string> avgExitPrice;
    std::optional<std::string> realizedPnl;
    std::optional<std::string> commission;
    std::optional<std::string> commissionAsset;
    std::optional<int64_t> firstTradeTime;
    std::optional<int64_t> lastTradeTime;
};
```

### 5.C Snapshot / View Types

```cpp
// source: src/orders/order_result.h

struct OrderView {
    OrderIdentity identity;                         // variant<NormalOrderIdentity, AlgoOrderIdentity>
    NormalOrderSnapshot normal;
    std::optional<OrderMetadata> metadata;          // from journal, may be nullopt
    std::optional<OrderFillSummary> fills;          // populated only if explicitly fetched
};

struct OrderPoolSnapshot {
    OrderPoolKind kind;                             // Open | History
    std::chrono::system_clock::time_point capturedAt;
    std::vector<OrderView> orders;

    size_t count() const;
    std::span<const OrderView> items() const;       // preferred iteration
    std::optional<OrderView> atSnapshotIndex(size_t index) const;   // MQL4 migration only
    std::optional<OrderView> byIdentity(const OrderIdentity&) const;
};
```

INVARIANT: `OrderPoolSnapshot` is treated as a point-in-time snapshot by service APIs and never auto-refreshes.  
NOTE: struct fields are public; caller-side mutation is technically possible.  
INVARIANT: `atSnapshotIndex` order is snapshot-local; not stable across refreshes.  
INVARIANT: getters on `OrderView` never issue REST calls.

### 5.D Identity Types

```cpp
// source: src/orders/order_common.h

struct NormalOrderIdentity {
    Symbol symbol;                          // always required
    std::optional<int64_t> orderId;
    std::optional<ClientOrderId> clientOrderId;
    bool operator==(const NormalOrderIdentity&) const = default;
};

struct AlgoOrderIdentity {
    Symbol symbol;                          // always required
    std::optional<int64_t> algoId;
    std::optional<ClientAlgoId> clientAlgoId;
    bool operator==(const AlgoOrderIdentity&) const = default;
};

using OrderIdentity = std::variant<NormalOrderIdentity, AlgoOrderIdentity>;
```

INVARIANT: `symbol` is always required in every identity operation.  
INVARIANT: `NormalOrderIdentity` and `AlgoOrderIdentity` are NOT interchangeable.

### 5.E Metadata and Config

```cpp
// source: src/orders/order_common.h

struct OrderMetadata {
    std::optional<int64_t> magic;
    std::optional<std::string> comment;
    std::optional<std::string> strategyTag;
};

struct OrdersConfig {
    std::string clientIdNamespace;
    bool allowBestEffortJournal{false};
    ResponseType defaultResponseType{ResponseType::ACK};
    std::chrono::milliseconds recvWindow{5000};
    bool allowRawTimestampOverride{false};
    PositionMode positionMode{PositionMode::Unknown};
    std::shared_ptr<OrderJournal> journal;     // inject instance directly
    bool journalIsDurable{false};             // auto-create DurableOrderJournal
    std::string journalPath{"orders_journal.log"};
};
```

### 5.F Enums

```cpp
// source: src/orders/order_common.h
enum class PlacementState    { Accepted, Rejected, UnknownPendingReconcile };
enum class PositionMode      { Unknown, OneWay, Hedge };
enum class ResponseType      { ACK, RESULT };
enum class OrderLifecycle    { Normal, Algo };
enum class OrderPoolKind     { Open, History };

enum class OrderErrorCategory {
    Validation, Unsupported, ExchangeReject,
    RateLimit, Auth, Network, Timeout, CanceledBeforeSend,
    Parse, Journal, Unknown
};

// source: src/orders/order_result.h
enum class FillSummaryCompleteness { Unavailable, Partial, Complete };

// source: src/orders/order_drafts.h
enum class ProtectionKind    { StopLoss, TakeProfit };
struct CloseEntirePosition   {};     // tag — maps to Binance closePosition=true

// source: src/types/trade.h
enum class OrderSide         { Buy, Sell };
enum class OrderType         { Limit, Market, Stop, StopMarket, TakeProfit, TakeProfitMarket, TrailingStopMarket };
enum class TimeInForce       { GTC, IOC, FOK, GTX };
enum class PositionSide      { Both, Long, Short };
enum class WorkingType       { MarkPrice, ContractPrice };

// source: src/orders/order_validator.h
// ValidationIssue::Severity
enum class Severity          { Error, Warning, Skipped };
```

### 5.G Decimal String Types

```cpp
// source: src/orders/decimal_string.h
// Aliases over one shared DecimalString class
using Price         = DecimalString;
using Quantity      = DecimalString;
using TriggerPrice  = DecimalString;
```

INVARIANT: `DecimalString::parse` rejects scientific notation, leading sign, whitespace, NaN/inf.  
INVARIANT: `double` must NEVER be used at order boundaries. Always `DecimalString`.

---

## 6. MQL4 Adapter

Source: `src/orders/mql4_adapter.h`, `src/orders/mql4_adapter.cpp`  
Namespace: `orders::mql4`

### 6.A Operation Mapping

```cpp
enum class TradeOperation { Buy, Sell, BuyLimit, SellLimit, BuyStop, SellStop };

struct MappedOrderSendDraft {
    Symbol symbol;
    TradeOperation operation;
    Quantity quantity;                      // NOT lots — caller must convert externally
    std::optional<Price> price;
    std::optional<Price> limitPrice;        // optional stop-limit execution price for BuyStop/SellStop
    std::optional<TriggerPrice> stopLoss;
    std::optional<TriggerPrice> takeProfit;
    std::optional<OrderMetadata> metadata;
};
```

| `operation` | Maps to | Notes |
|---|---|---|
| `Buy` | `Orders::market(side=Buy)` | |
| `Sell` | `Orders::market(side=Sell)` | |
| `BuyLimit` | `Orders::limit(side=Buy)` | `price` required |
| `SellLimit` | `Orders::limit(side=Sell)` | `price` required |
| `BuyStop` | `Orders::stopEntry(side=Buy)` | `price` → `triggerPrice`; `limitPrice` absent → STOP_MARKET |
| `SellStop` | `Orders::stopEntry(side=Sell)` | same as BuyStop |

### 6.B Protection Attachment

After entry `Accepted`, if `stopLoss` or `takeProfit` present:

1. Determine `positionSide` and `closeSide` from operation direction.
2. Call `Orders::protection(ProtectionOrderDraft{...})` per protection leg.
3. On protection failure: entry result still returned; warning added to `validation.issues`.
   - Warning codes: `mql4_stop_loss_attach_failed`, `mql4_stop_loss_attach_rejected`,
     `mql4_take_profit_attach_failed`, `mql4_take_profit_attach_rejected`

INVARIANT: quantity in protection uses entry quantity (not `CloseEntirePosition`).  
INVARIANT: adapter never accepts bare `double lots` — requires caller to pass `Quantity`.  
INVARIANT: no global selected-order state.

### 6.C Snapshot Helper

```cpp
Mql4Adapter::getOpenOrders(std::optional<Symbol> symbol = nullopt)
  → OrdersResult<OrderPoolSnapshot>
```
- Delegates to `Orders::openNormalOrderSnapshot`

---

## 7. Journal Rules

Source: `src/orders/order_journal.h`, `src/orders/order_journal.cpp`

### 7.A Configuration Matrix

| `allowBestEffortJournal` | `journalIsDurable` | `journal` (injected) | Behavior |
|---|---|---|---|
| `false` | `false` | `nullptr` | `NormalOrderService`: blocked (`-90006`); `AlgoOrderService`: continues without journal |
| `false` | `true` | `nullptr` | `DurableOrderJournal` auto-created at `journalPath` |
| `false` | `false` | durable instance | Uses injected durable journal |
| `false` | `false` | in-memory instance | Blocked (`-90009`): durable required |
| `true` | `false` | `nullptr` | `InMemoryOrderJournal` auto-created |
| `true` | any | any | Whichever journal is configured |

INVARIANT: when a journal is active, journal write occurs before REST send.  
INVARIANT: for both services, non-durable journal is blocked when `!allowBestEffortJournal` (`-90009`).

### 7.B DurableOrderJournal File Format

Tab-separated, one record per line. Append-only. `fsync`/`_commit` after each write.

```
R <correlationId> <symbol> <clientOrderId> <orderCategory> <side:int> <type:int>
  <positionSide:int> <quantity> <price> <requestParams> <sendTimestampMs> <state:int>
  <binanceOrderId|empty> <magic|empty> <comment|empty> <strategyTag|empty>

U <correlationId> <state:int> <binanceOrderId|empty> <responseTimestampMs>
```

Enum encoding (fragile — do not change enum order):

| Enum | 0 | 1 | 2 |
|---|---|---|---|
| `OrderSide` | Buy | Sell | |
| `OrderType` | Limit | Market | Stop | (continues to 6)
| `PositionSide` | Both | Long | Short |
| `PlacementState` | Accepted | Rejected | UnknownPendingReconcile |

INVARIANT: `metadata` (magic, comment, strategyTag) is persisted and recovered on restart.  
WARNING: enum-as-int encoding breaks if enum order changes.

### 7.C Metadata Redaction Rules

Used by `formatOrderView(view, includeMetadata)`:

| Field | `includeMetadata=false` | `includeMetadata=true` |
|---|---|---|
| `magic` | printed as `magic=<value>` | printed |
| `comment` | `metadata=[REDACTED]` | printed |
| `strategyTag` | `metadata=[REDACTED]` | printed |
| API keys, signatures | NEVER printed | NEVER printed |
| `timestamp`, raw REST params | NEVER printed | NEVER printed |

---

## 8. File Index

| Symbol | Source file |
|---|---|
| `Orders` | `src/orders/orders.h`, `orders.cpp` |
| `NormalOrderService` | `src/orders/normal_order_service.h`, `.cpp` |
| `AlgoOrderService` | `src/orders/algo_order_service.h`, `.cpp` |
| `orders::mql4::Mql4Adapter` | `src/orders/mql4_adapter.h`, `.cpp` |
| `NormalOrderIdentity`, `AlgoOrderIdentity`, `OrderIdentity` | `src/orders/order_common.h` |
| `OrderMetadata`, `OrdersConfig`, `PlacementState`, `OrderErrorCategory` | `src/orders/order_common.h` |
| `MarketOrderDraft`, `LimitOrderDraft`, `CloseByMarketDraft` | `src/orders/order_drafts.h` |
| `AmendLimitOrderDraft`, `StopEntryDraft`, `ProtectionOrderDraft` | `src/orders/order_drafts.h` |
| `NormalOrderDraft` (variant) | `src/orders/order_drafts.h` |
| `NormalPlacementResult`, `NormalCancelResult`, `NormalOrderSnapshot` | `src/orders/order_result.h` |
| `BatchPlacementResult`, `OrderFillSummary`, `OrderView`, `OrderPoolSnapshot` | `src/orders/order_result.h` |
| `formatOrderView` | `src/orders/order_result.cpp` |
| `ValidationReport`, `ValidationIssue` | `src/orders/order_common.h` |
| `OrderJournal`, `InMemoryOrderJournal`, `DurableOrderJournal` | `src/orders/order_journal.h`, `.cpp` |
| `OrderValidator` | `src/orders/order_validator.h`, `.cpp` |
| `OrderMapper` | `src/orders/order_mapper.h`, `.cpp` |
| `OrderIdGenerator` | `src/orders/order_id_generator.h`, `.cpp` |
| `IRestClient` | `src/orders/irest_client.h` |
| `RestClientAdapter` | `src/orders/rest_client_adapter.h`, `.cpp` |
| `Price`, `Quantity`, `TriggerPrice` (DecimalString) | `src/orders/decimal_string.h` |
| `OrderSide`, `OrderType`, `TimeInForce`, `PositionSide`, `WorkingType` | `src/types/trade.h` |
| `UserTrade` | `src/types/trade.h` |
