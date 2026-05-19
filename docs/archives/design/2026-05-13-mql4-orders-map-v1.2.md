# MQL4 Trading Functions To `src/orders` Mapping

**Version:** 1.2  
**Date:** 2026-05-13  
**Status:** ✅ DONE - Implemented  
**Disposition:** APPROVED — architectural review findings resolved

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.2 | 2026-05-13 | Resolved architectural review blockers: reconciled amend API shape, metadata storage, protection order quantity semantics, fill completeness, thread safety, snapshot index semantics, rate-limit guidance, and redaction spec |
| 1.1 | 2026-05-13 | Architectural review added (Section 14): 3 blockers identified, 4 high/medium issues documented; version bumped to reflect review state |
| 1.0 | 2026-05-13 | Initial mapping proposal from official MQL4 trading functions to the project's Binance USD-M Futures `orders` layer |

---

## 1. Muc Tieu

De xuat cac ham can map them vao `src/orders` khi can chuyen tu tu duy MQL4 sang Binance USD-M Futures SDK hien tai.

Trong MQL4, `OrderSend`, `OrderClose`, `OrderModify`, `OrderDelete`, `OrderSelect` va cac getter nhu `OrderLots`, `OrderProfit`, `OrderTicket` duoc thiet ke quanh mot global selected order va mot ticket co y nghia gan nhu identity truc tiep. Trong repo nay, `src/orders` dang la typed async facade tren Binance REST, co `NormalOrderService`, `IRestClient`, `OrderMapper`, `OrderValidator`, `OrderJournal`, `DurableOrderJournal` va cac API normal orders.

Muc tieu khong phai clone API MQL4 1:1. Muc tieu la chon cac mapping co semantics dung, expose ro cac phan chi partial/derived, va chan cac mapping co the gay nham lan trong production trading.

---

## 2. Understanding Lock

### 2.1 Summary

- Dang de xuat bo sung design cho `src/orders`, khong implement code trong buoc nay.
- Nguoi dung chinh la strategy/trading engine hoac code migration tu MQL4 sang C++ Binance Futures.
- MQL4 trading functions duoc tham chieu tu official docs tai `https://docs.mql4.com/trading`.
- `src/orders` hien co ho tro normal `MARKET`, `LIMIT`, close-by-market, batch, cancel/query/open/all normal orders.
- Binance Futures khong co global selected order, khong co global ticket tuong duong MQL4, va khong gan SL/TP/magic/comment/swap truc tiep len normal order.
- Cac getter MQL4-style neu co phai doc tu explicit snapshot, khong duoc am tham goi REST moi lan.
- Tai lieu nay phai phan biet `supported`, `proposed`, `partial`, `derived`, va `unsupported`.

### 2.2 Assumptions

- Pham vi van la Binance USD-M Futures, khong gom MQL4 broker, Spot, hay COIN-M.
- Existing typed API trong `src/orders` tiep tuc la API chinh. MQL4-style adapter neu co chi la compatibility layer.
- `Quantity`, `Price`, `TriggerPrice` tiep tuc dung `DecimalString`; khong them `double` cho order boundary.
- Algo/conditional orders la lifecycle rieng, co `algoId` / `clientAlgoId`, khong tron vao normal `orderId`.
- Profit/commission/close price chi co the derived tu fill/trade data. Funding/swap khong duoc trinh bay nhu exact per-order MQL4 `OrderSwap`.

### 2.3 Non-Functional Requirements

- **Reliability:** identity phai luon kem `symbol`; timeout/ambiguous outcome phai reconcile bang `clientOrderId`.
- **Financial correctness:** khong implicit convert MQL4 `lots` sang Binance `quantity`.
- **Performance:** getter-style functions khong tao hidden N+1 REST calls; economics data phai fetch theo snapshot/batch.
- **Security/privacy:** `magic`, `comment`, strategy metadata trong journal/client id khong duoc log nhu exchange truth; signed params, signature, full query string phai redact.
- **Maintainability:** moi mapping phai co status ro rang va endpoint/lifecycle tuong ung.

### 2.4 Open Questions

- Co can compatibility namespace rieng nhu `orders::mql4` hay chi can typed migration helpers trong `src/orders`?
- Economics/fill summary nen nam trong `orders` hay module account/trades rieng roi `orders` chi compose snapshot?

These questions are not Phase A blockers. Metadata storage is resolved in Section 6.2.

---

## 3. References

| Source | URL | Notes |
|---|---|---|
| MQL4 Trade Functions | <https://docs.mql4.com/trading> | Official list of trading/order functions |
| MQL4 OrderSend | <https://docs.mql4.com/trading/ordersend> | Placement arguments include symbol, command, volume, price, slippage, SL/TP, comment, magic, expiration |
| MQL4 OrderSelect | <https://docs.mql4.com/trading/orderselect> | Copies selected order data; later getters return copied values |
| MQL4 OrderModify | <https://docs.mql4.com/trading/ordermodify> | Modifies price/SL/TP/expiration depending on order type |
| MQL4 Order Properties | <https://docs.mql4.com/constants/tradingconstants/orderproperties> | `OP_BUY`, `OP_SELL`, pending order constants |
| Binance New Order | <https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/New-Order> | Existing normal order placement base |
| Binance Modify Order | <https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/Modify-Order> | `PUT /fapi/v1/order`, LIMIT-only amendment |
| Binance New Algo Order | <https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/New-Algo-Order> | Stop, take profit, trailing stop lifecycle |
| Binance Account Trade List | <https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/Account-Trade-List> | Fill/trade data for derived economics |
| Binance Income History | <https://developers.binance.com/docs/derivatives/usds-margined-futures/account/rest-api/Get-Income-History> | Funding/income data; not exact MQL4 per-order swap |
| Existing orders design | `docs/archives/design/2026-05-12-orders-layer-v1.1.md` | Current `orders` architecture |
| Orders compliance review | `docs/archives/design/2026-05-13-orders-layer-v1.1-compliance-review.md` | Implementation status before this proposal |

---

## 4. Critical Semantic Differences

### 4.1 No Bare Ticket

MQL4 code often treats `OrderTicket()` as a globally useful identity. Binance USD-M `orderId` must not be exposed as a ticket-only identity in this SDK. Every normal order identity must carry:

```cpp
struct NormalOrderIdentity {
    Symbol symbol;
    std::optional<int64_t> orderId;
    std::optional<ClientOrderId> clientOrderId;
};
```

Algo order identity is separate:

```cpp
struct AlgoOrderIdentity {
    Symbol symbol;
    std::optional<int64_t> algoId;
    std::optional<ClientAlgoId> clientAlgoId;
};
```

A generic identity can be a variant:

```cpp
using OrderIdentity = std::variant<NormalOrderIdentity, AlgoOrderIdentity>;
```

Do not add `queryByTicket(ticket)` or `cancelByTicket(ticket)`.

### 4.2 Lots Are Not Quantity

MQL4 `volume` / `OrderLots()` are lots. Binance `quantity` is symbol-specific contract/base quantity represented as decimal text. The SDK must not silently convert lots to `Quantity`.

Allowed designs:

- Require caller to pass `Quantity` directly.
- Or require an explicit caller-provided `LotSizingPolicy` that uses exchange filters and strategy sizing rules.

Disallowed design:

- `double lots` accepted by `orders` and internally mapped to Binance quantity.

### 4.3 No Hidden Selected Order

MQL4 `OrderSelect()` stores copied order data in implicit runtime state, then `OrderProfit()`, `OrderType()`, etc. read from that selected copy. Core `src/orders` must not add hidden mutable selected-order state.

If compatibility is needed, expose explicit snapshot objects:

```cpp
struct OrderPoolSnapshot {
    std::chrono::system_clock::time_point capturedAt;
    std::vector<OrderView> orders;
};

struct OrderView {
    OrderIdentity identity;
    NormalOrderSnapshot normal;
    std::optional<OrderMetadata> metadata;
    std::optional<OrderFillSummary> fills;
};
```

Getter helpers may read from `OrderView`; they must not fetch REST internally.

### 4.4 SL/TP Are Not Normal Order Fields

MQL4 `OrderSend` and `OrderModify` carry `stoploss` and `takeprofit` fields on the trade/order. Binance Futures models stop loss, take profit, stop entry and trailing stop as separate conditional/algo orders with their own lifecycle and ids. A protection order may be linked locally to a primary order intent, but it is not an exchange-attached field on the normal order.

### 4.5 Market Price And Slippage Do Not Map Exactly

MQL4 market operations include `price` and `slippage`. Existing `Orders::market()` and `Orders::closeByMarket()` do not provide an equivalent max-slippage execution contract. Any compatibility API must either reject slippage parameters or document that slippage protection requires strategy-level price checks or different order types.

---

## 5. Mapping Matrix

Status meanings:

- **Current:** already covered by `src/orders`.
- **Proposed:** add typed API to `src/orders`.
- **Partial:** some semantics map, but not full MQL4 behavior.
- **Derived:** computed from snapshots, fills, metadata, or account data.
- **Unsupported:** no direct safe mapping.

| MQL4 function | Status | Current / proposed mapping | Notes |
|---|---|---|---|
| `OrderSend` `OP_BUY` / `OP_SELL` | Current, partial | `Orders::market(MarketOrderDraft)` | Requires `Quantity`; no implicit lots; no exact MQL4 price/slippage semantics |
| `OrderSend` `OP_BUYLIMIT` / `OP_SELLLIMIT` | Current, partial | `Orders::limit(LimitOrderDraft)` | Pending limit maps to normal LIMIT; expiration/GTD not currently modeled |
| `OrderSend` `OP_BUYSTOP` / `OP_SELLSTOP` | Proposed, partial | `AlgoOrderService::stopMarket/stopLimit` phase | Requires Binance algo lifecycle and `clientAlgoId` |
| `OrderClose` | Current, partial | `Orders::closeByMarket(CloseByMarketDraft)` for one-way mode | Closes by symbol/side/quantity, not by MQL4 ticket; hedge mode requires explicit position side design |
| `OrderCloseBy` | Unsupported | None | No direct Binance USD-M normal-order equivalent |
| `OrderDelete` pending limit | Current | `cancelNormalByOrderId` / `cancelNormalByClientOrderId` | Identity must include symbol |
| `OrderDelete` stop/TP/protection | Proposed, partial | `cancelAlgoByAlgoId` / `cancelAlgoByClientAlgoId` phase | Only after algo cancel/query scope exists |
| `OrderModify` limit price/quantity | Proposed, partial | `amendLimitOrderByOrderId` / `amendLimitOrderByClientOrderId` | Binance LIMIT-only amend; not MQL4 SL/TP modify |
| `OrderModify` SL/TP | Proposed, partial | Create/cancel/amend linked protection algo orders | Separate exchange orders, not fields on the parent normal order |
| `OrderSelect` | Proposed, partial | `OrderPoolSnapshot` / `OrderView` explicit selection helpers | No global selected state; snapshot can be stale |
| `OrdersTotal` | Proposed | `OrderPoolSnapshot{kind=Open}.count()` over `openNormalOrders` and optional algo open snapshot | Counts snapshot entries only |
| `OrdersHistoryTotal` | Proposed, partial | `OrderPoolSnapshot{kind=History}.count()` over `queryAllNormal` windows | Subject to Binance retention/window limits |
| `OrderTicket` | Proposed, partial | `OrderIdentity` | Do not return a bare global ticket |
| `OrderSymbol` | Proposed | `OrderView::identity.symbol` | Snapshot field |
| `OrderType` | Proposed, partial | `Mql4TradeOperation` derived from Binance type/side/lifecycle | Algo orders need separate mapping |
| `OrderLots` | Derived | `OrderView::normal.origQty` or explicit lot policy result | Do not call this `lots` unless a lot policy exists |
| `OrderOpenPrice` | Derived | normal `price`; fill average for market/filled orders from trades | LIMIT price is not fill price |
| `OrderOpenTime` | Current/derived | `NormalOrderSnapshot::time` | Exchange timestamp |
| `OrderClosePrice` | Derived | `OrderFillSummary::avgExitPrice` | Requires user trades/fills |
| `OrderCloseTime` | Derived | final fill/cancel update time from order/trades | Semantics differ for canceled/expired orders |
| `OrderProfit` | Derived | realized PnL from user trades, optional unrealized from positions | Not a simple normal-order field |
| `OrderCommission` | Derived | commission sum from user trades | Requires symbol/order query and retention awareness |
| `OrderSwap` | Unsupported direct, derived account-level only | Funding/income history may be shown separately | Do not expose as exact per-order swap |
| `OrderStopLoss` | Proposed, partial | linked protection order trigger price | Local relationship plus algo order state |
| `OrderTakeProfit` | Proposed, partial | linked protection order trigger price | Local relationship plus algo order state |
| `OrderExpiration` | Proposed, partial | GTD/goodTillDate when supported by order type | Current `OrderRequest` has no GTD field |
| `OrderComment` | Proposed local metadata | `OrderMetadata::comment` in journal/store | Not guaranteed exchange field |
| `OrderMagicNumber` | Proposed local metadata | `OrderMetadata::magic` in journal/store | Not guaranteed exchange field |
| `OrderPrint` | Proposed utility | `formatOrderView(OrderView)` | Must redact metadata/signed data if logged |

---

## 6. Proposed Additions

### 6.1 Typed Identity

Add symbol-scoped identity types before adding MQL4-like helpers:

```cpp
enum class OrderLifecycle {
    Normal,
    Algo
};

struct NormalOrderIdentity {
    Symbol symbol;
    std::optional<int64_t> orderId;
    std::optional<ClientOrderId> clientOrderId;
};

struct AlgoOrderIdentity {
    Symbol symbol;
    std::optional<int64_t> algoId;
    std::optional<ClientAlgoId> clientAlgoId;
};

using OrderIdentity = std::variant<NormalOrderIdentity, AlgoOrderIdentity>;
```

Validation rules:

- `symbol` is always required.
- At least one id is required.
- Normal and algo identities are not interchangeable.
- Journal lookup by client id must also include symbol when ambiguity matters.

### 6.2 Local Metadata

MQL4 `magic` and `comment` should be modeled as local strategy metadata:

```cpp
struct OrderMetadata {
    std::optional<int64_t> magic;
    std::optional<std::string> comment;
    std::optional<std::string> strategyTag;
};
```

Storage decision for Phase A:

- Extend `JournalEntry` with `std::optional<OrderMetadata> metadata`.
- Persist metadata in `DurableOrderJournal` together with the order intent.
- Copy metadata into `OrderView` when constructing snapshots from journal-backed identities.
- Do not add a separate `OrderMetadataStore` in Phase A; that can be revisited only if metadata needs outlive or cross-cut journal entries.

This metadata is not exchange truth. It can disappear or diverge if journal retention, manual exchange actions, or import/reconcile logic is incomplete.

### 6.3 Limit Amendment

Add Binance-native amend API. Do not call it `OrderModify` in core `orders`.

```cpp
struct AmendLimitOrderDraft {
    NormalOrderIdentity identity;
    OrderSide side;
    Quantity quantity;
    Price price;
    std::optional<ResponseType> responseType;
    std::optional<int64_t> recvWindow;
};

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>>
amendLimitOrder(AmendLimitOrderDraft draft);

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>>
amendLimitOrderByOrderId(Symbol symbol,
                         OrderSide side,
                         int64_t orderId,
                         Quantity quantity,
                         Price price,
                         std::optional<ResponseType> responseType = std::nullopt,
                         std::optional<int64_t> recvWindow = std::nullopt);

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>>
amendLimitOrderByClientOrderId(Symbol symbol,
                               OrderSide side,
                               ClientOrderId clientOrderId,
                               Quantity quantity,
                               Price price,
                               std::optional<ResponseType> responseType = std::nullopt,
                               std::optional<int64_t> recvWindow = std::nullopt);
```

Implementation rule:

- `amendLimitOrder(AmendLimitOrderDraft)` is the canonical service path.
- The `ByOrderId` and `ByClientOrderId` overloads are convenience wrappers that construct `AmendLimitOrderDraft`.
- Optional `responseType` and `recvWindow` must be preserved by all overloads; no wrapper may silently drop them.
- `side` is required because Binance `PUT /fapi/v1/order` requires `side` in amend requests.

Required endpoint support:

- `IRestClient::modifyOrder(...)`
- `RestClient::modifyOrder(...)`
- `NormalOrderService::amendLimitOrder...`

Required warnings in API docs:

- Binance modify is for LIMIT orders only.
- Both `quantity` and `price` are required.
- Amending reorders queue priority.
- Some amendments can cancel the order, for example invalid partial-fill quantity reduction or GTX immediate execution.
- Ambiguous modify outcome must be reconciled by query.

### 6.4 Algo / Protection Orders

To cover MQL4 `OP_BUYSTOP`, `OP_SELLSTOP`, `stoploss`, and `takeprofit`, add a separate `AlgoOrderService` phase rather than overloading normal orders.

Candidate drafts:

```cpp
struct StopEntryDraft {
    Symbol symbol;
    OrderSide side;
    Quantity quantity;
    TriggerPrice triggerPrice;
    std::optional<Price> limitPrice;
    WorkingType workingType{WorkingType::ContractPrice};
    std::optional<ClientAlgoId> clientAlgoId;
    std::optional<OrderMetadata> metadata;
};

struct CloseEntirePosition {};

struct ProtectionOrderDraft {
    Symbol symbol;
    PositionSide positionSide;
    OrderSide closeSide;
    TriggerPrice triggerPrice;
    std::variant<Quantity, CloseEntirePosition> closeQuantity;
    std::optional<ClientAlgoId> clientAlgoId;
    std::optional<OrderMetadata> metadata;
};
```

Mapping rules:

- `Quantity` maps to an algo/protection order with explicit `quantity`.
- `CloseEntirePosition` maps to Binance `closePosition=true`.
- A caller cannot provide both `quantity` and `closePosition` in the typed API.
- Raw params must not override this invariant.

Protection orders must be documented as linked local intents, not exchange-attached child fields on the parent order. Canceling or amending a normal order must not silently mutate linked protection orders unless the wrapper exposes that behavior explicitly and journals each action.

### 6.5 Explicit Snapshot / Selector

Add explicit snapshot utilities for MQL4-like iteration:

```cpp
enum class OrderPoolKind {
    Open,
    History
};

struct OrderPoolSnapshot {
    OrderPoolKind kind;
    std::chrono::system_clock::time_point capturedAt;
    std::vector<OrderView> orders;

    size_t count() const;
    std::span<const OrderView> items() const;
    std::optional<OrderView> atSnapshotIndex(size_t index) const;
    std::optional<OrderView> byIdentity(const OrderIdentity& identity) const;
};
```

Rules:

- `items()` is the preferred API for iteration.
- `atSnapshotIndex` exists only for MQL4 `SELECT_BY_POS` style migration and must be documented as snapshot-local index access.
- `atSnapshotIndex` uses snapshot order only; do not promise exchange deterministic ordering across refreshes or exchange responses.
- `OrderView` is stale immediately after capture.
- Getter helpers read only from `OrderView`.
- Refresh requires an explicit new query call.

Snapshot refresh guidance:

- Refresh is explicit and rate-limit-aware; no getter may refresh internally.
- Callers should prefer symbol-scoped snapshots over account-wide snapshots.
- Polling loops must use caller-configured cadence/backoff and respect `RateLimiter` state.
- `OrderPoolSnapshot` construction is a point-in-time composition of REST responses and journal metadata; it is not an atomic exchange-wide transaction.

### 6.6 Fill And Economics Summary

Add derived fill/economics types only after REST support exists for user trades:

```cpp
enum class FillSummaryCompleteness {
    Unavailable,
    Partial,
    Complete
};

struct OrderFillSummary {
    Symbol symbol;
    int64_t orderId{0};
    FillSummaryCompleteness completeness{FillSummaryCompleteness::Unavailable};
    std::string executedQty;
    std::optional<std::string> avgEntryPrice;
    std::optional<std::string> avgExitPrice;
    std::optional<std::string> realizedPnl;
    std::optional<std::string> commission;
    std::optional<std::string> commissionAsset;
    std::optional<int64_t> firstTradeTime;
    std::optional<int64_t> lastTradeTime;
};
```

Constraints:

- Binance `userTrades` is symbol-scoped and can be filtered by `orderId`.
- Query windows and retention limits must be respected.
- Funding/income history is account/symbol/time data, not exact MQL4 per-order swap.
- Getter helpers like `orderProfit(view)` must return unavailable/partial when fill data is absent.
- Empty strings must not encode missing economics data; use `std::optional` and `FillSummaryCompleteness`.

### 6.7 Optional MQL4 Adapter

If a migration wrapper is needed, keep it separate:

```cpp
namespace orders::mql4 {

enum class TradeOperation {
    Buy,
    Sell,
    BuyLimit,
    SellLimit,
    BuyStop,
    SellStop
};

struct MappedOrderSendDraft {
    Symbol symbol;
    TradeOperation operation;
    Quantity quantity;
    std::optional<Price> price;
    std::optional<Price> limitPrice;
    std::optional<TriggerPrice> stopLoss;
    std::optional<TriggerPrice> takeProfit;
    std::optional<OrderMetadata> metadata;
};

} // namespace orders::mql4
```

This adapter must not accept `double lots` unless a caller-supplied lot policy is present. It must not expose hidden selected-order state.

---

## 7. Function-Level Recommendations

### 7.1 Add Now Or Next Normal-Order Phase

1. `OrderIdentity` and `OrderMetadata`.
2. `amendLimitOrderByOrderId` / `amendLimitOrderByClientOrderId`.
3. `OrderPoolSnapshot` / `OrderView` over existing `openNormalOrders` and `queryAllNormal`.
4. Formatting helper equivalent to `OrderPrint`, named `formatOrderView`.

`formatOrderView` redaction rules:

- Never print API keys, signatures, signed query strings, `timestamp`, or raw REST params.
- Metadata fields `comment` and `strategyTag` must be redacted by default as `[REDACTED]`; a caller may opt in to include them for local debug output only.
- `magic` may be printed because it is numeric strategy routing metadata, but logs must label it as local metadata, not exchange data.
- If redaction policy is configurable, the default must be the safest policy above.

### 7.2 Add With Algo Phase

1. Stop entry mapping for `OP_BUYSTOP` / `OP_SELLSTOP`.
2. Protection order mapping for SL/TP.
3. Algo cancel/query/open APIs.
4. Snapshot integration for normal plus algo orders.

### 7.3 Add With Trades/Economics Phase

1. `userTrades(symbol, orderId, startTime, endTime)` support in REST/client adapter.
2. `OrderFillSummary`.
3. Derived `profit`, `commission`, `closePrice`, `closeTime` helpers that report unavailable when data is missing.

### 7.4 Keep Unsupported

1. Direct `OrderCloseBy` mapping.
2. Exact per-order `OrderSwap`.
3. Bare ticket-only query/cancel.
4. Hidden process-global selected order.
5. Implicit lot conversion.

---

## 8. Example Migration Shape

MQL4-style intent:

```mql4
int ticket = OrderSend(Symbol(), OP_BUYLIMIT, 0.10, price, 3,
                       stoploss, takeprofit, "mean-revert", 42, 0);
OrderModify(ticket, newPrice, stoploss, takeprofit, 0);
OrderSelect(ticket, SELECT_BY_TICKET);
double profit = OrderProfit();
```

Proposed C++ shape:

```cpp
LimitOrderDraft entry{
    .symbol = "BTCUSDT",
    .side = OrderSide::Buy,
    .quantity = Quantity::parse("0.010").value(),
    .price = Price::parse("62000.00").value(),
    .timeInForce = TimeInForce::GTC,
};

auto placed = co_await orders.limit(std::move(entry));
if (!placed || placed->state != PlacementState::Accepted) {
    co_return;
}

NormalOrderIdentity identity{
    .symbol = placed->symbol,
    .orderId = placed->orderId,
    .clientOrderId = placed->clientOrderId,
};

// Not MQL4 OrderModify. This is Binance LIMIT amend only.
auto amended = co_await orders.amendLimitOrderByOrderId(
    identity.symbol,
    OrderSide::Buy,
    *identity.orderId,
    Quantity::parse("0.010").value(),
    Price::parse("61950.00").value());

// SL/TP are separate protection orders in the future AlgoOrderService phase.
ProtectionOrderDraft stopLoss{/* ... */};
ProtectionOrderDraft takeProfit{/* ... */};

auto snapshot = co_await orders.openNormalOrderSnapshot(Symbol{"BTCUSDT"});
auto view = snapshot.byIdentity(identity);
if (view && view->fills) {
    const auto realized = view->fills->realizedPnl;
}
```

The example intentionally uses `Quantity`, not `lots`, and passes `symbol` on every identity operation.

---

## 9. Error Handling And Reconcile

- New placement and amend calls must keep `UnknownPendingReconcile` semantics.
- Ambiguous amend/cancel result must be resolved by querying `symbol + orderId/clientOrderId`.
- Protection order placement must journal parent-child local relationship before send when durable journal is required.
- Derived economics helpers must distinguish:
  - unavailable because data was not loaded,
  - partial because only some fills are available,
  - complete within known Binance retention/window constraints.

Do not hide exchange errors behind MQL4-like boolean success. Return `OrdersResult<T>` and preserve `BinanceError` / order-aware categories.

### 9.1 Thread Safety And Concurrency

- `OrderJournal` implementations used by `orders` must be safe for concurrent coroutine access from a multi-threaded `io_context`.
- Journal write/update paths must serialize internal state with a mutex, strand, or equivalent mechanism.
- `OrderPoolSnapshot` is immutable after construction and safe to read concurrently.
- Snapshot construction is not exchange-atomic; it composes one or more REST responses plus local journal metadata at `capturedAt`.
- Concurrent amend/query/cancel on the same `OrderIdentity` is allowed, but callers must treat REST responses as potentially stale and reconcile ambiguous outcomes by querying the same identity.
- The SDK must not use process-global selected-order state, so concurrent MQL4-style selection races are structurally avoided.

---

## 10. Testing Strategy

Unit tests:

- `OrderIdentity` validation rejects missing symbol and empty ids.
- No ticket-only APIs exist in public `orders` headers.
- MQL4 adapter rejects `lots` unless an explicit sizing policy is provided.
- `amendLimitOrder` requires `quantity` and `price` and maps exact params.
- Snapshot getters do not call `IRestClient`.
- `OrderView` helper returns unavailable for profit/commission when fills are absent.
- `OrderMetadata` persists and redacts expected fields.
- `atSnapshotIndex` is documented and tested as snapshot-local access only.
- `OrderFillSummary` uses optional fields and completeness enum for missing/partial data.
- `ProtectionOrderDraft` cannot represent both explicit `Quantity` and `CloseEntirePosition`.

Integration-style tests with fake REST:

- Ambiguous amend error returns `UnknownPendingReconcile` and journal state is updated.
- Amend by `orderId` and by `clientOrderId` send correct Binance params.
- Open/history snapshots are timestamped and stable after construction.
- Derived fill summary groups trades by `symbol + orderId`.
- Protection order phase journals parent-child links and does not silently cancel/modify linked orders without explicit API call.
- Concurrent amend and query on the same identity do not corrupt journal state.
- Snapshot staleness isolation: a snapshot does not update itself after later placements, and `capturedAt` remains the capture time.
- Snapshot refresh tests assert callers trigger explicit refreshes and getter helpers never perform hidden REST calls.

No test should hit production Binance by default.

---

## 11. Phased Delivery

### Phase A - Identity, Metadata, Snapshot

- Add `OrderIdentity`, `OrderMetadata`, `OrderView`, `OrderPoolSnapshot`.
- Extend `JournalEntry` and `DurableOrderJournal` to persist `OrderMetadata`.
- Build snapshot helpers over existing `openNormalOrders` and `queryAllNormal`.
- Add formatting helper equivalent to `OrderPrint`.

### Phase B - Limit Amend

- Add `modifyOrder` to `IRestClient` and `RestClient`.
- Add canonical `amendLimitOrder(AmendLimitOrderDraft)` plus `amendLimitOrderByOrderId` and `amendLimitOrderByClientOrderId` wrappers to `NormalOrderService` and `Orders`.
- Add journal/reconcile handling for ambiguous amend outcomes.

### Phase C - Algo / Protection Orders

- Implement `AlgoOrderService`.
- Add stop-entry and protection order drafts with explicit `Quantity` vs. `CloseEntirePosition` semantics.
- Add algo query/cancel/open snapshot integration.

### Phase D - Fills / Economics

- Add account trade query support.
- Add `OrderFillSummary` with `FillSummaryCompleteness` and optional economics fields.
- Add derived helper functions for close price/time, realized profit, and commission.

### Phase E - Optional MQL4 Adapter

- Add `orders::mql4` namespace only after core typed APIs exist.
- Keep all dangerous differences explicit: no global ticket, no implicit lots, no hidden selected order.

---

## 12. Decision Log

| Decision | Alternatives Considered | Objections Raised | Resolution |
|---|---|---|---|
| Do not clone MQL4 global functions as core API | Add `OrderSend`, `OrderSelect`, getters directly | Hidden selected state is stale and unsafe in async C++ | Accepted; use typed APIs and optional adapter only |
| Require symbol-scoped identity | Bare `ticket` / `orderId` only | Binance normal and algo identities are not one universal ticket | Accepted; `OrderIdentity` is variant and symbol-scoped |
| Do not convert MQL4 lots implicitly | Accept `double lots` and multiply internally | Wrong quantity can cause material financial error | Accepted; require `Quantity` or explicit lot policy |
| Add LIMIT amend API | Treat existing cancel+new as enough | Binance has native modify endpoint; cancel+new changes semantics more | Accepted; name it `amendLimitOrder`, not `OrderModify` |
| Keep SL/TP as separate protection orders | Store SL/TP fields on normal order | Binance does not attach SL/TP fields to normal order | Accepted; local linked algo orders only |
| Keep getter helpers snapshot-only | Let getters fetch REST lazily | Hidden N+1 calls hurt rate limits and make data freshness unclear | Accepted; getters read `OrderView` only |
| Treat profit/commission as derived | Add fields to `NormalOrderSnapshot` | Requires trades/fills and can be incomplete | Accepted; use `OrderFillSummary` with completeness status |
| Mark `OrderCloseBy` unsupported | Approximate with two market orders | That is strategy behavior, not API equivalence | Accepted; unsupported direct mapping |
| Mark `OrderSwap` unsupported direct | Attribute funding to orders by time | Funding is not exact MQL4 per-order swap | Accepted; account-level/income derived only |
| Store magic/comment locally | Encode everything into `clientOrderId` | Length/privacy/collision constraints and exchange semantics mismatch | Accepted; journal metadata is primary |
| Persist metadata in `JournalEntry` for Phase A | Separate `OrderMetadataStore` | Separate store adds lookup and consistency complexity before there is a real need | Accepted; `DurableOrderJournal` persists metadata with intent |
| Use canonical `AmendLimitOrderDraft` | Only inline overload parameters | Draft preserves optional `responseType` and `recvWindow` without wrapper drift | Accepted; overloads construct the draft |
| Make protection quantity semantics type-level | `bool closePosition` plus optional quantity | Silent ignore of quantity with `closePosition=true` is dangerous | Accepted; use `variant<Quantity, CloseEntirePosition>` |
| Keep snapshot index access but rename it | Remove index access entirely | MQL4 `SELECT_BY_POS` migration needs position-style access, but ordering must be clearly snapshot-local | Accepted; use `atSnapshotIndex` and prefer `items()` iteration |
| Encode fill completeness in type | Empty strings for missing prices/PnL | Empty string cannot distinguish missing vs. real data absence | Accepted; use optional fields plus `FillSummaryCompleteness` |

---

## 13. Review And Arbitration

### Skeptic / Challenger

Accepted objections:

- MQL4 open market trades and Binance open normal orders are not the same pool.
- Bare ticket identity is unsafe.
- Lots vs quantity cannot be implicit.
- Market price/slippage semantics do not map exactly.
- `OrderModify` semantics are fragile because Binance modify is LIMIT-only and can reorder or cancel in edge cases.
- Local metadata and economics can diverge from exchange/account truth.
- Stop-order surface must wait for algo lifecycle.

No skeptic objections were rejected.

### Constraint Guardian

Accepted objections:

- Snapshot/selector must be explicit, timestamped, refreshable, and stale by design.
- Getter-style helpers must not cause hidden REST calls.
- Trade/economics data must respect rate limits, retention, and query windows.
- Logs/journals must redact signed material and sensitive strategy metadata.
- Mapping matrix must label direct, partial, derived, and unsupported mappings.

No constraint objections were rejected.

### User Advocate

Accepted objections:

- Avoid the word `ticket` in core API names.
- Do not name Binance LIMIT amend as `OrderModify`.
- Put "lots != quantity" near the top and in examples.
- Document SL/TP as separate linked orders.
- Show snapshot-style usage instead of implicit selected order.
- Mark unsupported mappings plainly.

No user-advocate objections were rejected.

### Arbiter Decision

The mapping proposal is acceptable as a design artifact. It gives useful migration guidance without pretending Binance USD-M Futures has MQL4's exact order model. Required guardrails are incorporated: symbol-scoped identity, no implicit lot conversion, no hidden selected order, narrow LIMIT amend semantics, separate algo/protection order lifecycle, and derived-only economics.

**Final disposition:** APPROVED.

---

## 14. Architectural Review

**Reviewer:** Architect Review  
**Date:** 2026-05-13  
**Verdict:** READY FOR IMPLEMENTATION — architectural review findings resolved in v1.2.

### 14.1 Strengths

**Symbol-scoped Identity (Sections 4.1, 6.1)**
`std::variant<NormalOrderIdentity, AlgoOrderIdentity>` tách biệt hai lifecycle ở type level. Binance `orderId` không unique across symbols — design này là phòng thủ đúng chống identity confusion trong production trading code.

**Explicit Snapshot thay vì Global State (Section 4.3)**
`OrderPoolSnapshot` với `capturedAt` timestamp là quyết định kiến trúc đúng nhất của tài liệu. MQL4's hidden `OrderSelect` state là thảm họa trong async C++20 coroutine context: giữa hai `co_await` point, coroutine khác có thể mutate state. Design này loại bỏ hoàn toàn race condition class đó.

**Phased Delivery (Section 11)**
Thứ tự A→B→C→D→E có dependency logic đúng: identity (A) phải có trước amend (B); trades REST support (D) phải có trước economics; adapter (E) wraps typed API, không phải ngược lại.

**Decision Log (Section 12)**
Documenting rejected alternatives với lý do cụ thể là thực hành tốt. "Wrong quantity can cause material financial error" đủ mạnh để enforce `Quantity` type boundary.

### 14.2 Resolved Blockers

**~~[BLOCKER-1]~~ `AmendLimitOrderDraft` and method signatures were inconsistent (Section 6.3) — RESOLVED**

Previous issue: the draft struct included `responseType` and `recvWindow`, while the convenience method signatures dropped those fields.

Resolution in v1.2:

- `amendLimitOrder(AmendLimitOrderDraft)` is the canonical implementation path.
- `amendLimitOrderByOrderId` and `amendLimitOrderByClientOrderId` are convenience wrappers.
- Optional `responseType` and `recvWindow` are preserved by all overloads.

**~~[BLOCKER-2]~~ `OrderMetadata` storage was undecided (Section 2.4) — RESOLVED**

Open question "extend `JournalEntry` hay tạo `OrderMetadataStore` riêng?" block Phase A vì `OrderView` có field `std::optional<OrderMetadata> metadata` — implementor không thể viết `OrderView` construction code mà không biết metadata đến từ đâu. Phải resolve trước khi bắt đầu Phase A.

Resolution in v1.2:

- Phase A extends `JournalEntry` with `std::optional<OrderMetadata> metadata`.
- `DurableOrderJournal` persists metadata with the intent record.
- `OrderView` copies metadata from journal-backed identity construction.
- A separate `OrderMetadataStore` is deferred until a real cross-journal need exists.

**~~[BLOCKER-3]~~ `ProtectionOrderDraft` had unsafe `closePosition` + `quantity` ambiguity (Section 6.4) — RESOLVED**

Previous issue: Binance API with `closePosition=true` ignores `quantity`, so a design that allowed both values would have created a financial footgun.

Resolution in v1.2:

- `ProtectionOrderDraft` now uses `std::variant<Quantity, CloseEntirePosition> closeQuantity`.
- A caller cannot represent both explicit quantity and close-entire-position in the typed API.
- Raw params are not allowed to override the invariant.

### 14.3 Resolved Issues

**~~[HIGH]~~ `OrderFillSummary::avgExitPrice` type did not encode completeness (Section 6.6) — RESOLVED**

Previous issue: `std::string` fields could not distinguish missing fill data from present-but-empty values.

Resolution in v1.2:

- `OrderFillSummary` now has `FillSummaryCompleteness`.
- Economics fields that can be absent are `std::optional`.
- Empty string is no longer used to mean unavailable.

**~~[HIGH]~~ Thread safety model was unspecified — RESOLVED**

Tài liệu sử dụng `boost::asio::awaitable` nhưng không nói gì về:
- `OrderJournal` có thread-safe không khi nhiều coroutines cùng write?
- Concurrent amend + query trên cùng `orderId` có race condition không?
- `OrderPoolSnapshot` construction có atomic không?

Với single-threaded `io_context` thì ít vấn đề hơn, nhưng nếu dùng strand hoặc thread pool thì phải có explicit thread safety guarantee cho `OrderJournal` và journal write paths.

Resolution in v1.2:

- Section 9.1 now specifies journal thread-safety requirements.
- `OrderPoolSnapshot` is immutable after construction.
- Concurrent amend/query/cancel must reconcile by identity after ambiguous outcomes.

**~~[MEDIUM]~~ `byIndex` on `OrderPoolSnapshot` was a footgun (Section 6.5) — RESOLVED**

Previous issue: generic index lookup suggested stable ordering where no stable exchange ordering exists.

Resolution in v1.2:

- `items()` is the preferred iteration API.
- `atSnapshotIndex` is retained only for MQL4 `SELECT_BY_POS` migration.
- Index access is explicitly snapshot-local and not stable across refreshes.

**~~[MEDIUM]~~ Rate limit budget was not integrated into snapshot refresh model — RESOLVED**

Section 2.3 nói "getter functions không tạo hidden N+1 REST calls" (đúng), nhưng khi `OrderPoolSnapshot` stale, caller phải tạo new snapshot — mỗi lần đó là một REST call tốn rate limit weight. Cần document recommended refresh cadence, hoặc expose rate limit budget context để caller có thể tự throttle refresh.

Resolution in v1.2:

- Section 6.5 now requires explicit, caller-controlled refresh.
- Symbol-scoped snapshots are preferred.
- Polling loops must use caller-configured cadence/backoff and respect `RateLimiter`.

**~~[LOW]~~ `formatOrderView` redaction spec was unclear (Section 7.1) — RESOLVED**

"Must redact metadata/signed data if logged" — cần specify rõ: redact fields nào, format nào (mask toàn bộ? chỉ log key không log value? thay bằng `[REDACTED]`?). Nếu để implementor tự quyết sẽ dẫn đến inconsistent redaction across codebase và potential log leakage.

Resolution in v1.2:

- Section 7.1 defines default redaction rules.
- `comment` and `strategyTag` are redacted as `[REDACTED]` by default.
- Signed material, raw REST params, signatures, API keys, and timestamps are never printed.

### 14.4 Testing Strategy Additions — Incorporated (Section 10)

The architectural review requested the following test cases, now included in Section 10:

1. **Concurrent amend race:** Gửi amend và query cùng lúc — đảm bảo `UnknownPendingReconcile` được return đúng và journal state không corrupt.
2. **Snapshot staleness isolation:** Sau khi `OrderPoolSnapshot` được tạo, có order mới được placed — đảm bảo snapshot không tự động update và `capturedAt` phản ánh đúng thời điểm capture.

### 14.5 Review Summary

| Hạng mục | Trạng thái |
|---|---|
| Identity design (`NormalOrderIdentity`, `AlgoOrderIdentity`, variant) | Approved |
| Explicit snapshot / no global selected state | Approved |
| Phased delivery order (A→B→C→D→E) | Approved |
| No implicit lot conversion | Approved |
| SL/TP as separate protection orders | Approved |
| MQL4 adapter in separate namespace, phase E | Approved |
| `AmendLimitOrderDraft` vs. method signatures | Resolved in Section 6.3 |
| `OrderMetadata` storage decision | Resolved in Section 6.2 |
| `ProtectionOrderDraft.closePosition` + `quantity` ambiguity | Resolved in Section 6.4 |
| `OrderFillSummary` completeness typing | Resolved in Section 6.6 |
| Thread safety model for `OrderJournal` | Resolved in Section 9.1 |
| Snapshot index semantics on `OrderPoolSnapshot` | Resolved in Section 6.5 |
| Rate limit guidance for snapshot refresh | Resolved in Section 6.5 |
| `formatOrderView` redaction spec | Resolved in Section 7.1 |

**Overall:** Tài liệu đủ điều kiện proceed sang implementation. Architectural review blockers have been resolved in v1.2 and no re-approval is required before Phase A.
