# Loss Manager Module — v1.1

**Version:** 1.1  
**Date:** 2026-05-18  
**Status:** DONE  
**Scope:** Global, one-way USD-M Futures positions tracked by `SignalEngine`

---

## Summary

Loss Manager monitors live ROI for tracked open positions and applies two protective actions:

- **BE trigger (`ROI <= -50%` by default):** move the existing take-profit to the position break-even price.
- **DCA trigger (`ROI <= -80%` by default):** place one same-direction market order with the original entry quantity, then move TP to the new break-even after Binance reports the combined position.
- After each successful DCA finalization, BE/DCA thresholds reset against the updated combined position.
- When `max_dca_count` is reached, no more DCA is placed; TP remains at the latest BE and the position is not force-closed.
- Stop-loss and trailing-stop orders are not modified by this module.
- The module runs from the existing `position_check_interval_seconds` monitor loop.

This version is implementation-ready only for one-way mode (`PositionSide::Both`), matching the current `OrdersConfig{.positionMode = PositionMode::OneWay}` in `src/main.cpp`.

---

## Binance API Facts Validated

| Fact | Implementation impact |
|---|---|
| `PUT /fapi/v1/order` supports modifying `LIMIT` orders only. | BE TP should be a normal reduce-only `LIMIT`, then amend existing TP instead of cancel+replace where possible. |
| `POST /fapi/v1/order` supports `MARKET` and `LIMIT`. | DCA uses `Orders::market()`. BE TP placement uses `Orders::limit()`. |
| USD-M conditional TP/SL algo orders use `/fapi/v1/algoOrder`. | `Orders::protection()` is not used by Loss Manager v1.1. Existing orders-layer algo support should be fixed separately if SL/trailing relies on it. |
| `GET /fapi/v2/positionRisk` returns `breakEvenPrice`, `entryPrice`, `markPrice`, `positionAmt`, `leverage`, and `positionSide`. | Add `breakEvenPrice` to `Position`; use it as the primary BE source. |
| `GET /fapi/v1/commissionRate` is per-symbol and weight 20. | Do not fetch one global fee at startup. Use `breakEvenPrice`; fallback to configured fee only if BE is unavailable. |

References:

- [Binance Modify Order](https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/Modify-Order): `PUT /fapi/v1/order`
- [Binance New Order](https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/New-Order): `POST /fapi/v1/order`
- [Binance New Algo Order](https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/New-Algo-Order): `POST /fapi/v1/algoOrder`
- [Binance Position Information V2](https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/Position-Information-V2): `GET /fapi/v2/positionRisk`
- [Binance User Commission Rate](https://developers.binance.com/docs/derivatives/usds-margined-futures/account/rest-api/User-Commission-Rate): `GET /fapi/v1/commissionRate`

---

## Config

Add a top-level section to `config.json`:

```json
"loss_manager": {
  "enabled": false,
  "roi_be_threshold": -0.50,
  "roi_dca_threshold": -0.80,
  "max_dca_count": 2,
  "fallback_taker_fee_rate": 0.0004,
  "allow_dca_on_recovered_positions": false,
  "dca_pending_timeout_cycles": 3
}
```

| Field | Type | Default | Rule |
|---|---:|---:|---|
| `enabled` | bool | `false` | Opt-in only. |
| `roi_be_threshold` | double | `-0.50` | Must be `< 0`. |
| `roi_dca_threshold` | double | `-0.80` | Must be `<= roi_be_threshold`. |
| `max_dca_count` | int | `2` | Must be `>= 0`; `0` disables DCA. |
| `fallback_taker_fee_rate` | double | `0.0004` | Used only when `breakEvenPrice <= 0`. |
| `allow_dca_on_recovered_positions` | bool | `false` | Prevents over-DCA after restart by default. |
| `dca_pending_timeout_cycles` | int | `3` | Pending DCA warning threshold; no automatic second DCA while pending. |

Invalid config is fail-closed: log an error and disable Loss Manager.

---

## Required Code Changes

| File | Change |
|---|---|
| `src/types/account.h` | Add `double breakEvenPrice{0.0};` to `Position`. |
| `src/rest/rest_client.cpp` | Parse `breakEvenPrice` in `parsePosition()`. |
| `src/engine/position_tracker.h/.cpp` | Add tracker mutation helpers for TP updates, quantity/entry refresh, and snapshot recovery marking. |
| `src/engine/signal_engine.h` | Add `LossManagerConfig`, `LossManager` member, and `IOrdersPort::amendLimitOrder(...)`. |
| `src/engine/signal_engine.cpp` | Instantiate Loss Manager and call it from the position monitor loop after reconciliation and before time-exit. |
| `src/engine/loss_manager.h/.cpp` | New module with state machine and order logic. |
| `src/main.cpp` | Parse `loss_manager`, pass config into `SignalEngine`; no startup commission fetch. |
| `tests/test_loss_manager.cpp` | New focused unit tests. |
| `tests/test_signal_engine.cpp` | Integration tests for monitor-loop wiring and tracker mutation. |

---

## Data Model

### `Position`

```cpp
struct Position {
    std::string symbol;
    PositionSide positionSide{PositionSide::Both};
    double positionAmt{0.0};
    double entryPrice{0.0};
    double breakEvenPrice{0.0};
    double markPrice{0.0};
    double unrealizedProfit{0.0};
    double liquidationPrice{0.0};
    int leverage{0};
    std::string marginType;
    double isolatedMargin{0.0};
    double initialMargin{0.0};
    double maintMargin{0.0};
    double notional{0.0};
};
```

### `TrackedPosition` additions

```cpp
struct TrackedPosition {
    // existing fields...
    bool recoveredFromSnapshot{false};
};
```

Add methods:

```cpp
bool updateTakeProfit(
    std::string_view symbol,
    int64_t tpOrderId,
    std::string tpClientOrderId);

bool clearTakeProfit(std::string_view symbol);

bool refreshPositionView(
    std::string_view symbol,
    double entryPrice,
    double quantity);

void markRecoveredFromSnapshot(std::string_view symbol);
```

`loadFromSnapshot()` sets `recoveredFromSnapshot = true` for positions restored on startup. Positions opened by `openPosition()` keep the default `false`.

### Loss manager state

```cpp
struct LossManagerConfig {
    bool enabled{false};
    double roiBeThreshold{-0.50};
    double roiDcaThreshold{-0.80};
    int maxDcaCount{2};
    double fallbackTakerFeeRate{0.0004};
    bool allowDcaOnRecoveredPositions{false};
    int dcaPendingTimeoutCycles{3};
};

struct LossManagerState {
    double originalQuantity{0.0};
    int dcaCount{0};

    bool beCurrent{false};
    double beEntryPrice{0.0};
    double bePositionAmt{0.0};
    double bePrice{0.0};

    bool dcaPending{false};
    int dcaPendingCycles{0};
    int64_t dcaOrderId{0};
    std::string dcaClientOrderId;
    double positionAmtBeforeDca{0.0};
    double entryPriceBeforeDca{0.0};
};
```

`beCurrent` is true only when the current live `entryPrice` and `positionAmt` still match the BE TP that was last placed/amended. If either changes, the next BE check can run again.

---

## Architecture

```text
SignalEngine::monitorTimeExit()
  every position_check_interval_seconds
    snapshot(includePositions=true)
    reconcile tracker with snapshot
    lossManager.evaluate(snapshot, tracker)
    processExpiredPositions(now)

LossManager::evaluate(snapshot, tracker)
  prune state for symbols no longer tracked
  for each tracked position
    find live Position by symbol and non-zero positionAmt
    refresh tracker quantity/entry from live position
    initialize per-symbol state if missing
    finalize pending DCA if live position amount increased
    compute ROI from live position
    if DCA threshold hit and DCA is allowed:
      run risk checks
      place DCA market order
      mark dcaPending
      stop processing this symbol until DCA finalizes
    if BE threshold hit and BE TP is not current:
      amend/place normal reduce-only LIMIT TP at BE
```

`LossManager::evaluate()` is a coroutine because all order actions use `IOrdersPort` awaitables.

```cpp
class LossManager {
public:
    LossManager(
        LossManagerConfig config,
        IOrdersPort& orders,
        IOrderCapPort& orderCap,
        IExposurePort& exposure,
        PositionTracker& tracker);

    boost::asio::awaitable<void> evaluate(
        const account::AccountSnapshot& snapshot,
        double availableBalance);
};
```

---

## ROI Calculation

Use Binance-style leveraged ROI:

```cpp
double calcRoi(const Position& pos) {
    if (pos.entryPrice <= 0.0 || pos.positionAmt == 0.0 || pos.leverage <= 0) {
        return 0.0;
    }
    const double direction = pos.positionAmt > 0.0 ? 1.0 : -1.0;
    return ((pos.markPrice - pos.entryPrice) / pos.entryPrice)
        * static_cast<double>(pos.leverage)
        * direction;
}
```

Examples with 10x:

- Long entry 100, mark 95 => ROI = -50%.
- Short entry 100, mark 105 => ROI = -50%.

---

## Break-Even Price

Primary source:

```cpp
if (pos.breakEvenPrice > 0.0) {
    return pos.breakEvenPrice;
}
```

Fallback only for tests or unexpected API response:

```cpp
double fallbackBreakEven(double entryPrice, bool isLong, double takerFeeRate) {
    if (isLong) {
        return entryPrice * (1.0 + takerFeeRate) / (1.0 - takerFeeRate);
    }
    return entryPrice * (1.0 - takerFeeRate) / (1.0 + takerFeeRate);
}
```

Rounding must never turn BE into a realized loss:

| Position | Close order | BE rounding |
|---|---|---|
| Long | `SELL LIMIT` | Round **up** to tick. |
| Short | `BUY LIMIT` | Round **down** to tick. |

Use the current live absolute `positionAmt` as TP quantity, rounded down to `stepSize`.

---

## BE TP Action

BE TP is a normal reduce-only `LIMIT`, not an algo/protection order.

### Existing TP is known

Use amend-first:

```cpp
auto amended = co_await orders.amendLimitOrder(AmendLimitOrderDraft{
    .identity = NormalOrderIdentity{
        .symbol = pos.symbol,
        .orderId = tracked.tpOrderId > 0 ? std::optional<int64_t>{tracked.tpOrderId} : std::nullopt,
        .clientOrderId = !tracked.tpClientOrderId.empty()
            ? std::optional<ClientOrderId>{tracked.tpClientOrderId}
            : std::nullopt,
    },
    .side = closeSide(pos),
    .quantity = closeQty,
    .price = bePrice,
});
```

Rules:

- If amend succeeds, keep the same TP identity and mark BE current.
- If amend fails with order-not-found, clear tracked TP and place a new TP.
- If amend fails for any other reason, leave the old TP untouched and do not mark BE current.

### No known TP

Place a new reduce-only limit:

```cpp
auto placed = co_await orders.limit(LimitOrderDraft{
    .symbol = pos.symbol,
    .side = closeSide(pos),
    .quantity = closeQty,
    .price = bePrice,
    .timeInForce = TimeInForce::GTC,
    .positionSide = PositionSide::Both,
    .reduceOnly = true,
    .clientOrderId = makeLossManagerTpClientId(pos.symbol),
});
```

If placement succeeds, call `tracker.updateTakeProfit(...)` with the returned order identity and mark BE current.

Do not cancel an existing TP before a replacement is known to be accepted.

---

## DCA Action

DCA order:

- Side: same as current position (`BUY` for long, `SELL` for short).
- Quantity: `state.originalQuantity`.
- Order type: `MARKET`.
- `positionSide`: `BOTH`.
- `reduceOnly`: unset/false.
- `responseType`: `RESULT` when supported so market fill status and avg price are available earlier.

Before placing DCA, run existing risk gates:

```cpp
const double dcaNotional = state.originalQuantity * pos.markPrice;
auto cap = orderCap.check(dcaNotional, snapshot, tracker);
auto exposure = exposure.check(symbol, direction, dcaNotional, tracker, snapshot, availableBalance);
```

Rules:

- `OrderCapDecision::Block` blocks DCA.
- `ExposureDecision::Block` blocks DCA.
- `ExposureDecision::ScaleDown` blocks DCA in v1.1; Loss Manager does not silently change the fixed DCA size.
- If DCA is blocked or rejected, do not set `dcaPending`; still attempt BE TP if BE threshold is hit.
- If DCA is accepted, set `dcaPending = true`, persist order identity in memory, and do not amend/place BE TP in the same cycle. The next BE TP update must use the combined live position after DCA finalization.

Recovered startup positions:

- If `tracked.recoveredFromSnapshot == true` and `allow_dca_on_recovered_positions == false`, DCA is disabled for that symbol.
- BE TP management still runs for recovered positions.

---

## DCA Finalization

Do not rely only on `entryPrice` changes. Finalize when the live position shows the expected amount increase. `qtyTolerance` is derived from the symbol `stepSize`.

```cpp
const double expectedAbsQty =
    std::abs(state.positionAmtBeforeDca) + state.originalQuantity;

const bool amountIncreased =
    std::abs(pos.positionAmt) + qtyTolerance >= expectedAbsQty;
```

On finalization:

1. Increment `dcaCount`.
2. Clear `dcaPending`.
3. Refresh tracker entry/quantity from the live position.
4. Mark `beCurrent = false`.
5. Call BE TP action using the new live `breakEvenPrice`.

If `dcaPending` remains true for more than `dca_pending_timeout_cycles`, log a warning containing symbol, client order id, order id, old amount, and current amount. Do not place another DCA while pending.

---

## State Lifecycle

```text
Position tracked
  init state:
    originalQuantity = abs(tracked.quantity)
    dcaCount = maxDcaCount if recovered DCA is disabled else 0

ROI above BE threshold
  no action

ROI <= BE threshold
  if BE TP is not current:
    amend normal TP to BE, or place normal TP if none exists

ROI <= DCA threshold
  if dcaCount < maxDcaCount and no pending DCA and risk gates allow:
    place same-direction market DCA
    dcaPending = true
    skip BE update until DCA finalizes
  else:
    ensure BE TP is current when possible

DCA pending
  wait for live position amount increase
  finalize DCA
  amend/place BE TP for combined position

Position flat or tracker removed
  erase LossManagerState for that symbol
```

---

## Integration Details

### `IOrdersPort`

Add:

```cpp
virtual boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>>
amendLimitOrder(AmendLimitOrderDraft draft) = 0;
```

`OrdersPort` delegates to `Orders::amendLimitOrder()`.

### `SignalEngine::monitorTimeExit`

Refactor to avoid duplicate position snapshots:

```cpp
boost::asio::awaitable<void> SignalEngine::monitorTimeExit() {
    while (m_running) {
        m_timeExitTimer.expires_after(m_config.positionCheckInterval);
        co_await m_timeExitTimer.async_wait(...);
        if (!m_running) co_return;

        account::AccountSnapshotRequest request;
        request.includePositions = true;
        auto snapshot = co_await m_account.snapshot(request);
        if (!snapshot) {
            log warning;
            continue;
        }

        co_await reconcileTrackedPositions(*snapshot);

        if (m_lossManager) {
            co_await m_lossManager->evaluate(*snapshot, snapshot->account.availableBalance);
        }

        co_await processExpiredPositions(std::chrono::system_clock::now());
    }
}
```

Keep the existing no-arg `reconcileTrackedPositions()` as a wrapper for tests if useful, but implement a snapshot-taking overload:

```cpp
boost::asio::awaitable<void> reconcileTrackedPositions(const account::AccountSnapshot& snapshot);
```

### `main.cpp`

Parse config only. Do not fetch commission rate at startup.

```cpp
engine::LossManagerConfig lossConfig;
const auto lossJson = config.value("loss_manager", nlohmann::json::object());
lossConfig.enabled = lossJson.value("enabled", false);
lossConfig.roiBeThreshold = lossJson.value("roi_be_threshold", -0.50);
lossConfig.roiDcaThreshold = lossJson.value("roi_dca_threshold", -0.80);
lossConfig.maxDcaCount = lossJson.value("max_dca_count", 2);
lossConfig.fallbackTakerFeeRate = lossJson.value("fallback_taker_fee_rate", 0.0004);
lossConfig.allowDcaOnRecoveredPositions =
    lossJson.value("allow_dca_on_recovered_positions", false);
lossConfig.dcaPendingTimeoutCycles =
    lossJson.value("dca_pending_timeout_cycles", 3);
```

Pass `lossConfig` into the primary `SignalEngine` constructor.

---

## Failure Handling

| Case | Behavior |
|---|---|
| Snapshot fails | Skip cycle; no order action. |
| BE price cannot be determined | Log error; no TP mutation. |
| Tick/step rounding fails | Log error; no order action. |
| Existing TP amend succeeds | Mark BE current. |
| Existing TP amend says order not found | Clear TP identity, place new normal limit TP. |
| Existing TP amend fails otherwise | Leave old TP untouched; do not mark BE current. |
| New BE TP placement fails | Leave tracker unchanged; retry next cycle if threshold still hit. |
| DCA risk gate blocks | Do not DCA; try BE TP if needed. |
| DCA market reject | Do not set pending; retry next cycle subject to risk gates. |
| DCA pending timeout | Warn only; do not place another DCA. |
| Bot restart with open positions | BE management allowed; DCA disabled unless explicitly configured. |

---

## Non-Goals

- No hedge-mode support.
- No per-strategy Loss Manager config.
- No Martingale or exponential sizing.
- No forced close after max DCA.
- No SL/trailing-stop mutation.
- No unconditional cancel+replace for TP when amend can be used.
- No global startup commission fetch.

---

## Decision Log

| # | Decision | Reason |
|---|---|---|
| D1 | BE TP is normal reduce-only `LIMIT`. | Existing strategy TP is normal limit; BE should not suffer market slippage. |
| D2 | Amend existing TP before fallback placement. | Avoids a protection gap from cancel-before-place. |
| D3 | Use `breakEvenPrice` from position API first. | Binance already computes per-position BE; avoids wrong global fee assumptions. |
| D4 | DCA goes through `OrderCap` and `ExposureController`. | DCA increases risk and must respect global caps. |
| D5 | ScaleDown from exposure blocks DCA. | Fixed-size DCA is explicit; silent resizing changes strategy semantics. |
| D6 | Recovered positions cannot DCA by default. | Prevents over-DCA after process restart with in-memory state. |
| D7 | Pending DCA finalizes on position amount increase. | More robust than checking entryPrice change only. |
| D8 | Loss Manager uses polling loop for v1.1. | Matches current engine loop and is sufficient for long-term strategies. |

---

## Test Plan

### Unit tests: `tests/test_loss_manager.cpp`

| Test | Expected |
|---|---|
| ROI long -49.9% | No order. |
| ROI long -50.0%, no TP | Place reduce-only SELL limit at rounded-up BE. |
| ROI short -50.0%, no TP | Place reduce-only BUY limit at rounded-down BE. |
| Existing TP and BE hit | Calls `amendLimitOrder`, not cancel. |
| Amend order-not-found | Places new limit TP and updates tracker. |
| Amend exchange reject | Leaves tracker TP unchanged and marks BE not current. |
| BE already current | No duplicate amend/place. |
| `breakEvenPrice > 0` | Uses Binance BE, ignores fallback fee. |
| `breakEvenPrice == 0` | Uses fallback fee formula. |
| ROI -80%, risk allows | Places same-direction market DCA with original quantity. |
| DCA accepted at -80% | Does not amend/place BE TP until DCA finalizes. |
| ROI -80%, order cap blocks | No DCA; BE TP still attempted. |
| ROI -80%, exposure blocks | No DCA; BE TP still attempted. |
| Exposure scale-down | No DCA in v1.1. |
| `max_dca_count = 0` | No DCA, BE still works. |
| Recovered position default | No DCA, BE still works. |
| Pending DCA amount unchanged | No second DCA. |
| Pending DCA amount increased | Finalizes DCA, increments count, updates BE TP. |
| Pending DCA timeout | Logs warning, no second DCA. |

### Integration tests: `tests/test_signal_engine.cpp`

| Test | Expected |
|---|---|
| Monitor loop with enabled Loss Manager | Snapshot is passed to Loss Manager once per position interval. |
| Tracker loaded from snapshot | `recoveredFromSnapshot=true`; DCA disabled by default. |
| BE update | Tracker TP identity is updated after new TP placement. |
| DCA finalize | Tracker quantity and entry are refreshed from live position. |
| Time-exit after BE move | Cancels the current BE normal TP identity. |

### REST/order tests

| Test | Expected |
|---|---|
| `parsePosition` with `breakEvenPrice` | Field is populated. |
| `IOrdersPort::amendLimitOrder` delegate | Calls `Orders::amendLimitOrder`. |
| BE TP limit draft | `reduceOnly=true`, `positionSide=BOTH`, side is opposite live position. |

---

## Numeric Example

Setup:

- Long BTCUSDT
- Entry = 100,000
- Binance `breakEvenPrice` = 100,080
- Leverage = 10x
- Original quantity = 0.01 BTC
- Tick = 0.10

| Event | Mark | ROI | Action | TP |
|---|---:|---:|---|---:|
| Open | 100,000 | 0% | Strategy TP exists | 110,000 |
| Price drops | 95,000 | -50% | Amend TP to BE | 100,080 |
| Price drops | 92,000 | -80% | Risk checks pass; DCA buy 0.01 | 100,080 until finalized |
| Binance updates position | 92,000 | current | Amount becomes 0.02, BE from API = 96,077 | Amend TP to 96,077 |
| Price drops again | 86,400 | -100% on entry 96,000 approx | 2nd DCA if allowed | BE updates after fill |
| Max reached | 82,000 | <= threshold | No more DCA | Last BE TP remains |

---

## Implementation Checklist

1. Add `breakEvenPrice` parsing.
2. Add `TrackedPosition::recoveredFromSnapshot` and tracker mutation helpers.
3. Add `IOrdersPort::amendLimitOrder`.
4. Implement `LossManagerConfig` parsing and validation.
5. Implement `LossManager` state machine.
6. Wire `LossManager` into `SignalEngine::monitorTimeExit`.
7. Add unit and integration tests from the tables above.
8. Run C++ test suite.
