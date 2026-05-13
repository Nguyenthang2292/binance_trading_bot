# Orders Command Reference

This document summarizes the actual API in `src/orders` based on the current implementation.

## 1) Scope

The `Orders` layer is split into two lifecycles:

- `NormalOrderService`: market/limit/close-by-market, amend, cancel/query/open/history, batch.
- `AlgoOrderService`: stop-entry/protection, cancel/query by algo identity, separate journal/reconcile semantics.

There is also `orders::mql4::Mql4Adapter`, which maps MQL4 operations to the typed API.

## 2) Normal Commands

| Command | Method | REST path |
|---|---|---|
| Place market | `Orders::market(MarketOrderDraft)` | `POST /fapi/v1/order` |
| Place limit | `Orders::limit(LimitOrderDraft)` | `POST /fapi/v1/order` |
| Close by market (one-way only) | `Orders::closeByMarket(CloseByMarketDraft)` | `POST /fapi/v1/order` |
| Amend limit by draft | `Orders::amendLimitOrder(AmendLimitOrderDraft)` | `PUT /fapi/v1/order` |
| Amend by orderId | `Orders::amendLimitOrderByOrderId(symbol, side, orderId, quantity, price, ...)` | `PUT /fapi/v1/order` |
| Amend by clientOrderId | `Orders::amendLimitOrderByClientOrderId(symbol, side, clientOrderId, quantity, price, ...)` | `PUT /fapi/v1/order` |
| Cancel by orderId | `Orders::cancelNormalByOrderId(...)` | `DELETE /fapi/v1/order` |
| Cancel by clientOrderId | `Orders::cancelNormalByClientOrderId(...)` | `DELETE /fapi/v1/order` |
| Cancel all by symbol | `Orders::cancelAllNormal(symbol)` | `DELETE /fapi/v1/allOpenOrders` |
| Query by orderId | `Orders::queryNormalByOrderId(...)` | `GET /fapi/v1/order` |
| Query by clientOrderId | `Orders::queryNormalByClientOrderId(...)` | `GET /fapi/v1/order` |
| Open orders | `Orders::openNormalOrders(optional<symbol>)` | `GET /fapi/v1/openOrders` |
| History | `Orders::queryAllNormal(...)` | `GET /fapi/v1/allOrders` |
| Batch normal (max 5) | `Orders::batchNormal(vector<NormalOrderDraft>)` | `POST /fapi/v1/batchOrders` |

## 3) Algo Commands

| Command | Method | Lifecycle |
|---|---|---|
| Stop entry | `Orders::stopEntry(StopEntryDraft)` | Algo placement flow + journal/reconcile |
| Protection (SL/TP style) | `Orders::protection(ProtectionOrderDraft)` | Algo placement flow + journal/reconcile |
| Cancel by algoId | `Orders::cancelAlgoByAlgoId(symbol, algoId)` | Algo cancel |
| Cancel by clientAlgoId | `Orders::cancelAlgoByClientAlgoId(symbol, clientAlgoId)` | Algo cancel |
| Query by algoId | `Orders::queryAlgoByAlgoId(symbol, algoId)` | Algo query |
| Query by clientAlgoId | `Orders::queryAlgoByClientAlgoId(symbol, clientAlgoId)` | Algo query |

Notes:

- `AlgoOrderService` uses placement state and ambiguous mapping like the normal flow:
  - `Accepted`
  - `Rejected`
  - `UnknownPendingReconcile`
- When the journal is configured, the algo intent is recorded with `orderCategory="algo"` and `metadata`.
- After `stopEntry/protection`, the placement result does not expose a separate `algoId`.
  Use `clientAlgoId` to query again via `queryAlgoByClientAlgoId(...)` when identity on the exchange is needed.

## 4) Strict Validator Contract

`OrderValidator` currently enforces:

- `reduceOnly=true` is rejected in `Hedge` mode (market + limit).
- Raw keys must match the following strict regex:
  - `^[A-Za-z][A-Za-z0-9_]{0,63}$`
- Raw blocked keys:
  - `symbol`, `side`, `type`, `quantity`, `price`, `positionSide`,
    `newClientOrderId`, `newOrderRespType`, `timeInForce`,
    `reduceOnly`, `clientAlgoId`.
- `recvWindow` / `timestamp` / `signature` in raw are blocked when
  `allowRawTimestampOverride=false`.
- Advisory issues are always attached to the report:
  - warning `position_mode_unknown` when `positionMode=Unknown`
  - warning `no_client_id_namespace` when `clientIdNamespace` is empty
  - skipped `exchange_info_unavailable` (exchange-rule checks are not backed by snapshot)

`NormalPlacementResult::validation` is always present on both success and failure paths.

## 5) MQL4 Adapter Mapping

`orders::mql4::Mql4Adapter::orderSend(MappedOrderSendDraft)` maps:

- `Buy/Sell` -> `Orders::market`
- `BuyLimit/SellLimit` -> `Orders::limit`
- `BuyStop/SellStop` -> `Orders::stopEntry`

Additional implemented behavior:

- `metadata` is not dropped: metadata is passed into the typed draft and the journal.
- `stopLoss`/`takeProfit` are not dropped:
  - after an entry is `Accepted`, the adapter creates separate protection orders via `Orders::protection`.
  - `stopLoss` maps to `ProtectionKind::StopLoss` (`STOP_MARKET`).
  - `takeProfit` maps to `ProtectionKind::TakeProfit` (`TAKE_PROFIT_MARKET`).
  - If attaching protection fails, the entry result is still returned, and a warning is added to `validation.issues`.
- `BuyStop/SellStop` support optional `limitPrice`:
  - no `limitPrice` -> stop-market (`STOP_MARKET`)
  - with `limitPrice` -> stop-limit (`STOP` + `price`)

## 6) Data and Snapshots

- `OrderPoolSnapshot`:
  - `count()`
  - `items()`
  - `atSnapshotIndex(...)`
  - `byIdentity(...)`
- `openNormalOrderSnapshot` / `queryAllNormalSnapshot` enrich metadata from the journal.
- `queryOrderFillSummary(symbol, orderId)` aggregates from `userTrades`.
  - uses `long double` accumulation in the current implementation (not multiprecision library).
  - marks the result as `Partial` if any trade parse fails or fill metadata is incomplete.

## 7) Journal Notes

- The normal flow enforces the durable-journal requirement when `allowBestEffortJournal=false`.
- The algo flow supports full journal/reconcile behavior when the journal is configured;
  without a journal, placement is still allowed to preserve current backward compatibility.
