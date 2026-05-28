# Take-Profit Reconciler Module — v1.0

**Version:** 1.0
**Date:** 2026-05-26
**Status:** Ready for Implementation
**Scope:** Global, one-way USD-M Futures positions tracked by `SignalEngine`; ensures every live position has TP coverage adopted, placed, or recognized after restart and during normal operation, and cleans up stale self-owned global TP orders.

---

## Summary

Today the global take-profit override is applied **only at the moment a position is opened** (`signal_engine.cpp:1688`). If the bot restarts, externally-opened positions exist, or a TP order is cancelled/filled, the live position runs without a TP until the next manual intervention. Loss Manager could amend TP, but it is disabled in `config.json:76–78`, so the production system currently has no safety net for the global TP invariant.

This module adds a **TakeProfitReconciler** that runs after each account snapshot inside the existing `monitorTimeExit` loop. For every live position that should have a global TP:

- If no eligible reduce-only close-side `LIMIT` order exists → **place** a new `gtp_` TP using the existing global-TP formula.
- If a self-owned global TP exists (`gtp_` or legacy `{symbol}_tp_`) → **adopt** it into the tracker.
- If only a Loss Manager (`lm_tp_`) or manual TP exists → **recognize coverage and skip placement**, but do **not** adopt it into the tracker and do **not** amend/cancel it.
- If a stale self-owned `gtp_` order exists for a flat symbol → **cancel** it with the same per-cycle order budget.
- (v2+) If a self-owned TP price/qty drifts beyond tolerance → **amend** it.

Default behavior is conservative: never mutate Loss Manager-owned TPs (`lm_tp_*`) or any other manually-placed TP. The reconciler only owns orders marked with `gtp_`; it can also adopt existing bot-created legacy TPs whose client order id matches `{symbol}_tp_...`, because those were created by the current global TP open-position path.

The reconciler also extends `reconcileTrackedPositions` from one-way (remove flat) to two-way (add unknown live positions, refresh `entryPrice`, `quantity`, `activeLeverage`). This fixes a latent bug independent of TP: positions opened externally or recovered from a snapshot are currently invisible to the rest of the engine.

---

## Problem Evidence

| # | File | Observation |
|---|------|-------------|
| 1 | [`config.json:71`](../../config.json) | `engine.take_profit.enabled=true`, `takeProfitPercent=5.0` |
| 2 | [`src/main.cpp:1080-1102`](../../src/main.cpp) | Config parsed into `engineConfig.takeProfitOverrideEnabled` / `…Percent`. Read once at startup, never re-read. |
| 3 | [`src/engine/signal_engine.cpp:1688-1724`](../../src/engine/signal_engine.cpp) | `shouldPlaceFixedTakeProfit` and `m_orders.limit(...)` only execute inside `openPosition` flow. |
| 4 | [`src/engine/position_tracker.cpp:9-31`](../../src/engine/position_tracker.cpp) | `loadFromSnapshot()` restores `symbol`, `direction`, `entryPrice`, `quantity` but **not** `activeLeverage`, TP order id, or any open exit orders. `TrackedPosition::activeLeverage{0}` survives; any recovered-position TP recomputation must use `Position.leverage` and skip positions with `leverage <= 0`. |
| 5 | [`src/engine/signal_engine.cpp:2038-2067`](../../src/engine/signal_engine.cpp) | `reconcileTrackedPositions(snapshot)` removes flat tracked positions but never adds live-but-untracked positions and never inspects open exit orders. |
| 6 | [`config.json:76-78`](../../config.json) | `loss_manager.enabled=false`. Loss Manager amend path is the only existing TP-mutation logic, and it is off. |
| 7 | [`src/engine/loss_manager.cpp:24,30`](../../src/engine/loss_manager.cpp) | Loss Manager uses client-order-id prefixes `lm_tp_` / `lm_sl_` — useful for distinguishing its orders. |
| 8 | [`src/engine/signal_engine.cpp:51-54`](../../src/engine/signal_engine.cpp) | Global TP currently uses `makeExitClientOrderId(symbol, "tp")` → `{symbol}_tp_{ts}`. **There is no marker that identifies an order as "managed by global TP system"**, so v1 cannot safely amend pre-existing TPs of unknown origin. |
| 9 | [`src/engine/signal_engine.h:56-69`](../../src/engine/signal_engine.h) | `IOrdersPort` exposes `limit`, `amendLimitOrder`, `protection`, etc., but **not** `openNormalOrders`. The concrete `Orders` class already provides it at [`src/orders/orders.h:62-63`](../../src/orders/orders.h). |
| 10 | [`src/types/account.h:21-36`](../../src/types/account.h) | Snapshot `Position` has `leverage` and `entryPrice` — both needed for TP-distance recomputation. |

---

## Non-goals

- **No SL reconciliation.** SL has its own placement semantics (`PROTECTION` algo orders) and is out of scope. `place_stop_loss=false` is a separate decision.
- **No Loss Manager interaction.** While LM is disabled, the reconciler must still respect `lm_tp_*` prefixes so re-enabling LM later cannot cause a coordination loop.
- **No true historical age enforcement for recovered positions.** Binance position snapshots do not expose position open time. v1 sets `openedAt=now` for recovered positions, so `max_hold_duration` resumes from recovery time. A future version can use order/trade history if exact age is required.
- **No portfolio-level changes** (sizing, leverage, exposure). The reconciler treats live positions as-is.

---

## Binance API Facts Validated

| Fact | Implementation impact |
|---|---|
| `GET /fapi/v1/openOrders` with no `symbol` filter returns all open normal orders for the account; weight ≈ 40. | Call once per reconcile cycle, index by symbol locally. Do **not** call per-symbol (`O(N)` weight). |
| `PUT /fapi/v1/order` modifies `LIMIT` orders only (qty and/or price). | Amend path uses `Orders::amendLimitOrder` — already in `IOrdersPort` at signal_engine.h:61. |
| `POST /fapi/v1/order` supports reduce-only `LIMIT`. | Reconciler uses `Orders::limit(...)` for new placements, same as the open-position path. |
| `clientOrderId` length 1–36, alphanumeric + `_-.`, must be unique while order is open. | New TP client order id format: `gtp_{sym12}_{ms36}_{seq2}`; max 30 chars. |

References:

- [Binance Current All Open Orders](https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/Current-All-Open-Orders): `GET /fapi/v1/openOrders`
- [Binance Modify Order](https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/Modify-Order): `PUT /fapi/v1/order`
- [Binance New Order](https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/New-Order): `POST /fapi/v1/order`

---

## Config

Add a sub-block under `engine.take_profit`:

```json
"engine": {
  "take_profit": {
    "enabled": true,
    "takeProfitPercent": 5.0,
    "reconciler": {
      "enabled": true,
      "mode": "adopt_or_place",
      "price_tick_tolerance": 1,
      "quantity_step_tolerance": 1,
      "max_orders_per_cycle": 8
    }
  }
}
```

| Field | Type | Default | Rule |
|---|---:|---:|---|
| `reconciler.enabled` | bool | `true` when `take_profit.enabled=true`, else `false` | Master switch. |
| `reconciler.mode` | string | `"adopt_or_place"` | One of `adopt_or_place` (v1) / `enforce_global` (v2+). |
| `reconciler.price_tick_tolerance` | int | `1` | v2+ amend threshold in price ticks. Ignored in v1. |
| `reconciler.quantity_step_tolerance` | int | `1` | v2+ amend threshold in qty steps. Ignored in v1. |
| `reconciler.max_orders_per_cycle` | int | `8` | Hard cap on placement/cancel/amend attempts per reconcile cycle. Failure-open. |

Invalid config is fail-closed: log an error and disable the reconciler.

**v1 ships with `mode=adopt_or_place` only.** `enforce_global` is reserved for v2 once we have an order marker (`gtp_` prefix) on every TP the reconciler has placed.

---

## Required Code Changes

| File | Change |
|---|---|
| `src/engine/signal_engine.h` | Add `IOrdersPort::openNormalOrders(std::optional<Symbol>) = 0;` declaration and the `OrdersPort` adapter pass-through. |
| `src/engine/position_tracker.h/.cpp` | In `loadFromSnapshot`, also set `tracked.activeLeverage = position.leverage`. Add `addRecovered(TrackedPosition)` for snapshot/external live positions and `refreshFromSnapshot(...)` to refresh `entryPrice`, `quantity`, and `activeLeverage`. Keep `updateTakeProfit(...)` signature unchanged in v1. |
| `src/engine/take_profit_reconciler.h/.cpp` | **New module**. Pure logic + boost::asio coroutine that takes a snapshot + tracker + orders port + symbol-info resolver. |
| `src/engine/signal_engine.h` | Add `TakeProfitReconcilerConfig` to `SignalEngine::Config` and an optional `TakeProfitReconciler` member. |
| `src/engine/signal_engine.cpp` | Two new call sites:<br>1. `run()` after `loadFromSnapshot` and the initial snapshot (line ~596).<br>2. `monitorTimeExit` after `reconcileTrackedPositions(*snapshotResult)` (line ~2034).<br>Extend `reconcileTrackedPositions` to add live-but-untracked positions and refresh `entryPrice/quantity/activeLeverage` for known ones. |
| `src/main.cpp` | Parse `engine.take_profit.reconciler`, instantiate config. |
| `tests/test_take_profit_reconciler.cpp` | New focused unit tests (table below). |
| `tests/test_signal_engine.cpp` | Integration tests for the wired-up `monitorTimeExit` path. |

---

## Data Model

### `TrackedPosition` (no new fields in v1)

All needed fields already exist:

```cpp
struct TrackedPosition {
    std::string symbol;
    strategy::Signal::Direction direction;
    double entryPrice{0.0};
    double quantity{0.0};
    int activeLeverage{0};            // FIXED: set in loadFromSnapshot from Position.leverage
    bool openingInFlight{false};      // skip when true
    bool recoveredFromSnapshot{false};
    // ...
};
```

v1 intentionally does **not** add `tpPrice`, `tpQuantity`, or `tpOwner`. Ownership is inferred from the open order's client order id during each reconcile pass. External TP coverage (`lm_tp_` or manual client ids) is never written into `TrackedPosition`, so existing time-exit and Loss Manager paths cannot cancel or amend external orders by mistake.

### `TakeProfitReconcilerConfig`

```cpp
namespace engine {

struct TakeProfitReconcilerConfig {
    bool enabled{false};
    enum class Mode { AdoptOrPlace, EnforceGlobal };
    Mode mode{Mode::AdoptOrPlace};
    int priceTickTolerance{1};
    int quantityStepTolerance{1};
    int maxOrdersPerCycle{8};
};

}  // namespace engine
```

### Client-order-id

```
gtp_{sym12}_{ms36}_{seq2}
```

Length budget: `gtp_` (4) + 12-char normalized symbol + `_` (1) + base36 epoch milliseconds capped at 10 chars + `_` (1) + 2-char cycle-local base36 sequence = max 30 chars. This stays below Binance's 36-char ceiling while preserving symbol-level traceability and uniqueness for multiple placements in the same millisecond.

Normalization rule for `sym12`: uppercase, keep only `[A-Z0-9_-]`, truncate to 12 chars. If normalization produces an empty string, fail placement for that symbol and log a warning.

---

## Architecture

```text
SignalEngine::run()
  initial AccountSnapshot
    PositionTracker::loadFromSnapshot(positions)   // FIX: also sets activeLeverage
  co_spawn TakeProfitReconciler::reconcileOnce(...)  // one-shot at startup

SignalEngine::monitorTimeExit()
  every position_check_interval_seconds
    snapshot(includePositions=true)
    reconcileTrackedPositions(snapshot)              // FIX: also adds live-but-untracked
    TakeProfitReconciler::reconcileOnce(snapshot)    // NEW
    lossManager.evaluate(snapshot, tracker)          // existing, disabled in prod today
    processExpiredPositions(now)
```

`TakeProfitReconciler::reconcileOnce` is a pure coroutine — no internal state, no timers. Idempotent by construction.

---

## Algorithm — v1 (`adopt_or_place`)

```text
1. Early exit if !cfg.enabled or m_engineConfig.takeProfitOverridePercent <= 0.

2. openOrdersResult = await m_orders.openNormalOrders(std::nullopt)
   On error: log warning, return (next cycle will retry).

3. Index open orders: ordersBySymbol[symbol] = vector<NormalOrderSnapshot>.

4. orderBudget = cfg.maxOrdersPerCycle

5. Stale self-owned cleanup:
   a. Build liveSymbols from snapshot positions where positionAmt != 0.
   b. For each open order with clientOrderId starts with "gtp_":
        - if order.symbol is not in liveSymbols:
            * if orderBudget == 0 → defer cleanup to next cycle
            * else cancel by orderId if present, otherwise clientOrderId
            * decrement orderBudget on cancel attempt
   c. Never cancel legacy "{symbol}_tp_", "lm_tp_", or manual orders in v1.

6. For each live position P in snapshot (positionAmt != 0):
   a. tracked = tracker.bySymbol(P.symbol)
   b. If tracked exists and tracked.openingInFlight → skip (new-position flow owns this).
   c. If P.leverage <= 0 → log warning, skip (cannot compute TP distance).

   d. Filter ordersBySymbol[P.symbol] to:
        - status == NEW or PARTIALLY_FILLED
        - type == LIMIT
        - reduceOnly == true
        - side == closeSide(direction_of_P)
        - remainingQty = parse(origQty) - parse(executedQty) > 0
      Call this list `candidates`.

   e. Classify candidates:
        - lmOwned: clientOrderId starts with "lm_tp_"
        - selfOwned: clientOrderId starts with "gtp_"
        - legacyGlobalOwned: clientOrderId starts with "{symbol}_tp_"
        - other: everything else (manual or unknown-origin TPs)

   f. Decision matrix (v1 mode = AdoptOrPlace):
        | State                                         | Action                                           |
        |-----------------------------------------------|--------------------------------------------------|
        | selfOwned or legacyGlobalOwned candidate       | Adopt largest remainingQty candidate into tracker; do NOT amend in v1. |
        | only lmOwned or other candidates exist         | Recognize external TP coverage; do NOT place; do NOT update tracker; do NOT amend/cancel. |
        | no candidate exists and orderBudget > 0        | Compute global TP price; place reduce-only LIMIT; update tracker; decrement orderBudget. |
        | no candidate exists and orderBudget == 0       | Skip symbol; log; next cycle retries. |

   g. Place flow (mirrors signal_engine.cpp:1688-1724):
        - direction = sign(P.positionAmt)
        - distance = entryPrice * percent / (100 * P.leverage)
        - tpPrice = direction==Long ? entry + distance (round DOWN to tick)
                                    : entry - distance (round UP to tick)
        - qty = quantityToStep(|P.positionAmt|, stepSize)
        - clientOrderId = makeGlobalTpClientOrderId(symbol)
        - On placement rejected/failed:
            * log Warning with binance code/message
            * DO NOT close the position
            * do not update tracker
            * proceed to next position

   h. Adopt flow:
        - tracker.updateTakeProfit(symbol, candidate.orderId, candidate.clientOrderId)
        - If tracker entry missing, addRecovered() first using snapshot data.

7. Return.
```

### Multi-candidate rationale

If a symbol has 2+ qualifying self-owned or legacy global open orders, v1 adopts the one with the largest **remaining** quantity (`origQty - executedQty`). Reasoning:

- Largest remaining quantity is most likely the dominant exit.
- Adopting one ≠ ignoring the others; they remain on the exchange and will reduce the position if hit.
- Amending or cancelling unknown orders is exactly the risk v1 avoids.
- Loss Manager and manual candidates are treated as external coverage only. They prevent duplicate `gtp_` placement but are not written into the tracker, because tracker ownership currently drives time-exit cancellation and Loss Manager amend behavior.

v2 will revisit with `enforce_global` semantics for self-owned `gtp_` orders only. Manual and Loss Manager orders remain out of scope unless a future owner field is added to `TrackedPosition`.

---

## Two-way `reconcileTrackedPositions`

Current (one-way, signal_engine.cpp:2038–2067) removes tracked entries whose live qty is 0. Extend to:

```cpp
boost::asio::awaitable<void> SignalEngine::reconcileTrackedPositions(const account::AccountSnapshot& snapshot) {
    std::unordered_map<std::string, const Position*> liveBySymbol;
    auto collect = [&](const std::vector<Position>& ps) {
        for (const auto& p : ps) if (std::abs(p.positionAmt) > 0) liveBySymbol[p.symbol] = &p;
    };
    if (snapshot.positions.has_value()) collect(*snapshot.positions);
    else collect(snapshot.account.positions);

    // 1. Remove flat tracked (existing behavior).
    for (const auto& tracked : m_tracker.all()) {
        if (!liveBySymbol.count(tracked.symbol)) {
            if (m_tracker.removeIfOpenedAt(tracked.symbol, tracked.openedAt)) {
                if (m_riskPort) m_riskPort->onPositionClosed(snapshot, nowMs());
                Logger::instance().log(LogLevel::Info,
                    "tracker reconciliation removed stale symbol=" + tracked.symbol);
            }
        }
    }

    // 2. Add live-but-untracked; refresh known ones (NEW).
    for (const auto& [symbol, p] : liveBySymbol) {
        auto existing = m_tracker.bySymbol(symbol);
        if (!existing) {
            TrackedPosition t;
            t.symbol = symbol;
            t.direction = p->positionAmt >= 0 ? strategy::Signal::Direction::Long
                                              : strategy::Signal::Direction::Short;
            t.entryPrice = p->entryPrice;
            t.quantity = std::abs(p->positionAmt);
            t.activeLeverage = p->leverage;
            t.openedAt = std::chrono::system_clock::now();  // snapshot has no true open time
            t.recoveredFromSnapshot = true;
            t.openingInFlight = false;
            m_tracker.addRecovered(std::move(t));
        } else if (!existing->openingInFlight) {
            m_tracker.refreshFromSnapshot(symbol, p->entryPrice, std::abs(p->positionAmt), p->leverage);
        }
    }
    co_return;
}
```

Notes:

- Refreshing tracker on partial fill (qty drift) is itself an independent bug fix — the engine currently believes the original quantity even after partial close.
- Recovered positions use `openedAt=now` because `Position` snapshots do not expose true open time. That means time-exit duration is measured from recovery time in v1.

---

## Interface Additions

### `IOrdersPort`

```cpp
// src/engine/signal_engine.h (and matching adapter in signal_engine.cpp)
virtual boost::asio::awaitable<OrdersResult<std::vector<NormalOrderSnapshot>>>
openNormalOrders(std::optional<Symbol> symbol = std::nullopt) = 0;
```

Adapter implementation:

```cpp
boost::asio::awaitable<OrdersResult<std::vector<NormalOrderSnapshot>>>
openNormalOrders(std::optional<Symbol> symbol) override {
    co_return co_await m_orders.openNormalOrders(std::move(symbol));
}
```

### `PositionTracker`

```cpp
// position_tracker.h
bool addRecovered(TrackedPosition pos);                 // bypass openingInFlight check
bool refreshFromSnapshot(std::string_view symbol,
                        double entryPrice,
                        double quantity,
                        int leverage);                  // false if not present or openingInFlight
bool updateTakeProfit(std::string_view symbol,
                      int64_t orderId,
                      std::string clientOrderId);       // existing signature; unchanged in v1
```

### `loadFromSnapshot` fix (independent of reconciler)

```cpp
// position_tracker.cpp:23 — ADD
tracked.activeLeverage = position.leverage;
```

This single line is a latent-bug fix and should be cherry-picked even if the reconciler is deferred.

---

## Failure Modes

| Failure | Reconciler behavior |
|---|---|
| `snapshot` call fails | Skip this cycle. Already handled by `monitorTimeExit`. |
| `openNormalOrders` call fails | Log Warning. Return early. Next cycle retries. **Do not** assume "no TPs exist" — that would cause duplicate placement. |
| `Position.leverage == 0` from API | Log Warning. Skip this position. **Do not** divide by zero. |
| Catalog tick/step missing for symbol | Log Warning. Skip. |
| `priceToTickDecimal` returns nullopt | Log Warning. Skip. |
| `m_orders.limit(...)` rejected (`PlacementState::Rejected`) | Log Warning with binance code+message. **Do not** close position. Tracker unchanged. |
| `m_orders.limit(...)` networking error | Log Warning. Tracker unchanged. Next cycle retries. |
| `orderBudget == 0` reached mid-loop | Log Info ("budget exhausted, deferring N symbols"). Next cycle retries. |
| Reconciler placed a TP, then position was closed externally before next cycle | The next reconcile pass sees a stale self-owned `gtp_` order for a flat symbol and cancels it, bounded by `max_orders_per_cycle`. Manual, legacy, and `lm_tp_` orders are not cancelled. |
| Two TPs end up on the same symbol (race between new-open and reconciler) | Both reduce-only LIMITs are safe — only one can fully fill before position is flat; the other gets auto-rejected. `openingInFlight` skip is the primary guard against this. |
| Manual or `lm_tp_` TP exists but tracker has no TP id | Reconciler recognizes external coverage and skips placement. Tracker remains without TP id so time-exit/Loss Manager cannot mutate an external order through tracker state. |

---

## Tests

### `test_take_profit_reconciler.cpp`

| # | Scenario | Expected |
|---|---|---|
| T1 | Recovered Long position, global TP enabled, no open orders | One `gtp_*` reduce-only LIMIT placed at `entry + entry*pct/(100*lev)` rounded DOWN to tick. Tracker updated. |
| T2 | Recovered Short position, same condition | One reduce-only LIMIT at `entry - distance` rounded UP. |
| T3 | Recovered position, one open `gtp_*` reduce-only close-side LIMIT | Order adopted into tracker; no new placement; no amend. |
| T4 | Recovered position, one legacy `{symbol}_tp_*` reduce-only close-side LIMIT | Order adopted into tracker; no new placement; no amend. |
| T5 | Recovered position, one manual custom `clientOrderId` candidate | External coverage recognized; no placement; tracker TP remains empty; no amend/cancel. |
| T6 | Recovered position with `openingInFlight=true` (race with new-open) | Reconciler skips. No order placed. |
| T7 | Global TP `enabled=false` or `percent <= 0` | No-op. |
| T8 | `reconciler.enabled=false` | No-op even if TP enabled. |
| T9 | `Position.leverage == 0` (API anomaly) | Skipped with Warning. No divide-by-zero. |
| T10 | `m_orders.limit()` returns `PlacementState::Rejected` | Position **not closed**. Tracker unchanged. Warning logged. |
| T11 | `openNormalOrders` returns error | Reconciler aborts; no orders placed. |
| T12 | `maxOrdersPerCycle=2`, 5 positions need TP | Exactly 2 placed; remaining 3 logged and deferred. |
| T13 | Order budget exhausted, mid-cycle position with existing self-owned TP | Adopt still happens (no placement consumed) — adoption is free. |
| T14 | Partial fill drift: live qty < tracker qty | `reconcileTrackedPositions` refresh updates tracker.qty. Reconciler treats current `Position.qty` as truth for new TP qty. |
| T15 | Two self-owned candidates where one is partially filled | Candidate with largest `origQty - executedQty` is adopted. |
| T16 | Stale `gtp_*` open order exists for a flat/missing live symbol | Reconciler cancels it within budget. |
| T17 | Stale manual, legacy `{symbol}_tp_*`, or `lm_tp_*` order exists for a flat/missing live symbol | Reconciler leaves it untouched. |
| T18 | Long max-length symbol client id generation | Generated `gtp_` client id is `<=36` chars and matches Binance regex. |
| T19 | Recovered position with one `lm_tp_*` candidate | External coverage recognized; no placement; tracker TP remains empty; no amend/cancel. |

### `test_signal_engine.cpp`

| # | Scenario | Expected |
|---|---|---|
| S1 | Engine startup with one externally-opened position and TP enabled | After the startup one-shot reconcile hook completes, a `gtp_*` LIMIT exists on the mock exchange for that symbol. |
| S2 | `monitorTimeExit` cycle where TP from previous cycle was filled (position now flat) | `reconcileTrackedPositions` removes tracked entry; reconciler is a no-op for that symbol. |
| S3 | New position opened in scan cycle, reconciler fires in the same `monitorTimeExit` tick | Reconciler sees `openingInFlight=true`, skips; new-open flow places the TP. |

---

## Rollout Plan

1. **Cherry-pick fix**: `loadFromSnapshot` sets `activeLeverage`. Ship independently — fixes a latent bug regardless of reconciler timeline.
2. **Phase 1 — Plumbing**: Add `IOrdersPort::openNormalOrders`, `PositionTracker::addRecovered/refreshFromSnapshot`. Keep existing `updateTakeProfit` signature. Extend `reconcileTrackedPositions` to two-way. Tests for tracker mutations.
3. **Phase 2 — Reconciler module**: Build `TakeProfitReconciler` with v1 algorithm (`adopt_or_place`). Unit tests T1–T19.
4. **Phase 3 — Wiring**: Add to `monitorTimeExit` after `reconcileTrackedPositions`. Add one-shot call in `run()` after initial snapshot. Integration tests S1–S3.
5. **Phase 4 — Production enable**: Ship with `reconciler.enabled=true` matching `take_profit.enabled`. Monitor for unexpected `gtp_*` placements via logs and an open-orders dashboard. Hold for one week before declaring stable.
6. **Phase 5 (future) — `enforce_global` mode**: Add v2 amend/cancel-replace logic for self-owned `gtp_*` orders only, gated on `mode=enforce_global`. Manual and `lm_tp_*` orders remain external unless tracker ownership is extended.

---

## Implementation Decisions

1. `reconciler.enabled` defaults to `take_profit.enabled`. Invalid reconciler config disables only the reconciler, not global TP placement for new positions.
2. v1 owns only `gtp_*` orders, and adopts only `gtp_*` or legacy `{symbol}_tp_*` orders into tracker state.
3. Manual and `lm_tp_*` orders count as external TP coverage. They prevent duplicate `gtp_` placement but are never written into tracker state.
4. Recovered position age is unknown. v1 sets `openedAt=now`; historical age enforcement is deferred.
5. Stale self-owned `gtp_*` orders for flat symbols are cancelled within `max_orders_per_cycle`; manual, legacy, and `lm_tp_*` stale orders are left untouched.

---

## Remaining Open Questions

1. **Hedge mode (`PositionSide::Long`/`Short` instead of `Both`)** — current bot is one-way only. If hedge mode is ever enabled, the candidate filter (`side == closeSide(direction)`) needs `positionSide` matching too.
2. **Should the reconciler emit metrics** (counters for placed/adopted/skipped/failed/cancelled)? Strongly recommended for production validation; defer to existing metrics infrastructure.

---

## References

- Loss Manager design — [`docs/design/2026-05-18-loss-manager-v1.0.md`](2026-05-18-loss-manager-v1.0.md) (TP amend semantics, `lm_tp_` prefix)
- Existing TP placement path — [`src/engine/signal_engine.cpp:1688-1724`](../../src/engine/signal_engine.cpp)
- Tracker snapshot recovery — [`src/engine/position_tracker.cpp:9-31`](../../src/engine/position_tracker.cpp)
- One-way reconciliation — [`src/engine/signal_engine.cpp:2038-2067`](../../src/engine/signal_engine.cpp)
