# Qlib Strategy Adapter Integration — Implementation Review v1.0

**Date:** 2026-05-21
**Status:** COMPLETE - superseded by v1.1 and v1.2
**Reviewer:** Brainstorming + structured code review
**Audience:** AI agents, human developers

**Design under review:**

- `docs/design/2026-05-21-qlib-strategy-adapters-v1.1.md`

**Predecessor design:**

- `docs/design/2026-05-21-qlib-strategy-adapters-v1.0.md`

**Files reviewed:**

New files (Phase 1, 2, 6, 7 implementation):

- `plugins/src/qlib_strategy_signal/CMakeLists.txt`
- `plugins/src/qlib_strategy_signal/strategy_qlib_strategy_signal.cpp`
- `src/engine/iexecution_planner.{h,cpp}`
- `src/engine/qlib_execution_planner.{h,cpp}`
- `tools/qlib_bridge/run_strategy.py`
- `tools/qlib_bridge/run_execution_plan_watcher.py`
- `tests/python/test_run_strategy.py`

Modified files:

- `CMakeLists.txt`
- `src/engine/position_tracker.h`
- `src/engine/signal_engine.{cpp,h}`
- `src/orchestration/batch_scheduler_thread.{cpp,h}`
- `src/orchestration/process_manager.{cpp,h}`
- `src/orchestration/qlib_state_store.cpp`
- `src/orchestration/runtime_ports.h`
- `src/orchestration/shadow_metrics_recorder.cpp`
- `src/orchestration/sqlite_helpers.h`
- `src/strategy/istrategy.h`
- `src/strategy/strategy_config.h`
- `tests/test_batch_scheduler_thread.cpp`

---

## 1. Executive Summary

The implementation **captures the architectural shape** of design v1.1 well: allowlist enforcement, schema versioning via `PRAGMA user_version`, atomic ready-flag via `run.status='succeeded'`, Decision Arbiter scaffolding in `SignalEngine`, IExecutionPlanner abstraction, and an async watcher daemon. The Phase 6 "ship abstraction with no behavior change" reordering is honored — `NativeExecutionPlanner` preserves current order placement.

However, the implementation falls short of design v1.1 in **three load-bearing areas**:

1. **`QlibExecutionPlanner` rejects every success case.** When the watcher generates a plan correctly, the planner explicitly returns error `-92002 "plan ready but live slice executor is not enabled"`. The async path is non-functional end-to-end.
2. **`run_strategy.py` never invokes a Qlib class.** It resolves the class name through the allowlist but only emulates TopK with a SQL `ORDER BY score DESC LIMIT topk`. `TopkDropoutStrategy` and `SoftTopkStrategy` produce identical output. `n_drop` and `trade_impact_limit` are not honored.
3. **Decision Arbiter has subtle bugs.** `runtimeState` is default-constructed and never populated, so `globalShadow` is always `false`. Aggregate exposure caps use `INT_MAX` / `infinity` placeholders. Per-adapter mode filtering and structured conflict logging are missing.

The remaining gaps are smaller (stale defaults wrong for sub-1h intervals, `universe_hash == "default"` silent bypass, schema drift between v1.1 design and implementation in `qlib_adapter_runtime_state` / `qlib_execution_*` tables, no promotion profiles, no per-slice revocation contract since no slice executor exists). These are all addressable.

This review concludes with a punch list of 14 next-step tasks (~400-600 LoC including tests) to close the gaps for a complete and reviewable Phase 1+2+5+6+7 implementation.

---

## 2. Decision-by-Decision Verification

The design v1.1 enumerated 13 binding decisions in its changelog (Section 0). This review verifies each.

| # | Decision | Status | Evidence |
|---|---|---|---|
| 1 | Async-only execution planner. C++ never invokes Python synchronously. | ⚠️ **Partial** | Watcher exists (`run_execution_plan_watcher.py`). C++ planner polls SQLite (`qlib_execution_planner.cpp:240-251`). However, when plan IS ready, planner returns error instead of consuming the plan. |
| 2 | Decision Arbiter in SignalEngine | ✅ **Implemented** with caveats | `signal_engine.cpp:531-674`, `signal_engine.h:135-155`. Adds candidates per `(symbol, interval)` and resolves via priority + confidence sort. See Critical Issue #3 for bugs. |
| 3 | SQLite contract: WAL, busy_timeout, atomic ready-flag, schema version, retry policy | ✅ **Mostly done** | `sqlite_helpers.h` has `readUserVersion` / `setUserVersion`. `run_strategy.py:56-73` sets PRAGMAs and version. `run_strategy.py:290-362` uses single transaction with `status='succeeded'` as final state. Missing: explicit 3-step retry (50ms→200ms→500ms) — relies on SQLite's `busy_timeout=5000ms` instead. |
| 4 | TWAP per-slice revocation contract | ❌ **Not implemented** | No slice executor exists in C++. `qlib_execution_slices` table has `revoked` status and `revoke_reason` columns (schema correct), but no code reads them, submits them, or marks them as revoked. |
| 5 | v1 TopK emits only `buy/hold/none`, `long/none` | ✅ **Enforced at schema + plugin** | `run_strategy.py:121-122`: CHECK constraints. `strategy_qlib_strategy_signal.cpp:417-426`: plugin rejects non-`buy` actions and non-`long` directions. |
| 6 | Per-adapter exposure caps (`max_concurrent_positions`, `max_total_risk_pct`) | ⚠️ **Partial** | `strategy_config.h:36-37`: fields added as `std::optional`. Plugin parses them (`strategy_qlib_strategy_signal.cpp:105-110`). Arbiter consumes them (`signal_engine.cpp:654-660`). Plugin itself doesn't pre-filter — relies on arbiter as sole gate. |
| 7 | New `qlib_adapter_runtime_state` table; legacy `qlib_runtime_state` untouched | ⚠️ **Created, schema differs** | `run_strategy.py:134-148`. Implementation has `active_run_id`, `state_version`, `updated_at_ms`, `rollback_reason`. Design v1.1 had `promotion_profile`, `promoted_at_ms`, `promoted_by`, `last_decision_at_ms`, `last_failure_at_ms`, `last_failure_reason`. Different intent: implementation optimizes for optimistic concurrency, design optimized for promotion lifecycle audit. |
| 8 | Promotion profiles per `qlib_class` | ❌ **Not implemented** | No `promotion_profiles` config section. No profile table. Plugin and arbiter have no concept of promotion thresholds. |
| 9 | Tighter stale defaults (`2 × interval`, `1.5 × interval`) | ⚠️ **Hardcoded constants** | `strategy_qlib_strategy_signal.cpp:38-39`: `maxArtifactAgeSeconds{7200}`, `maxDataAgeSeconds{3600}`. Correct for 1h interval but wrong for 30m (should be 3600 / 2700). |
| 10 | Universe hash validation in C++ | ⚠️ **Implemented with bypass** | `strategy_qlib_strategy_signal.cpp:162-200, 404-408`: schema + universe checked at construction and per evaluation. Both checks silently pass if either side equals `"default"`. Operators who forget `--universe-hash` get no protection. |
| 11 | Hardcoded allowlist (`frozenset` in Python) | ✅ **Correctly done** | `run_strategy.py:15-21`: `ALLOWED_CLASSES = frozenset({...})`. Lines 266-269: main rejects non-allowlisted classes. Also provides `CLASS_ALIASES` for short-form class names — acceptable since aliases are also source-only. |
| 12 | Rollout: Phase 6 (IExecutionPlanner) before Phase 4 | ✅ **Honored** | `iexecution_planner.h` defines `IExecutionPlanner` + `NativeExecutionPlanner`. `iexecution_planner.cpp` provides pass-through `co_await m_orders.market(...)`. No behavior change for existing strategies. `signal_engine.cpp:1240-1242` uses planner when present, falls back to direct `m_orders.market`. |
| 13 | Acceptance criteria 9, 10, 11 | ⚠️ **Partial** | (9) Arbiter exists ✓; (10) Schema check at plugin load ✓ (`strategy_qlib_strategy_signal.cpp:172-175`); (11) TWAP revocation **untested** because slice executor not implemented. |

**Verification result:** 4 ✅, 6 ⚠️, 3 ❌

---

## 3. Critical Issues

### 3.1 QlibExecutionPlanner rejects every success case

**Location:** `src/engine/qlib_execution_planner.cpp:240-255`

```cpp
while (orchestration::sqlite_helpers::nowMs() < deadlineMs) {
    if (planReady(db.get(), requestId, reason)) {
        markRequest(db.get(), requestId, "failed", "plan ready but live slice executor is not enabled");
        Logger::instance().log(
            LogLevel::Warning,
            "[QlibExecutionPlanner] Plan ready but slice executor disabled request_id=" + requestId);
        co_return std::unexpected(BinanceError::fromApiResponse(
            -92002,
            "qlib plan ready but live slice executor is not enabled"));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
}
markRequest(db.get(), requestId, "expired", reason);
co_return std::unexpected(BinanceError::fromApiResponse(
    -92001,
    "qlib execution plan not ready: " + reason));
```

**Problem:** When the watcher generates a plan correctly, the planner **rejects it**. There is no code path that calls `IOrdersPort::market` for slices. The planner can never place an order.

**Secondary problem:** `std::this_thread::sleep_for` inside a coroutine blocks the asio executor for 25ms × N iterations. This breaks the asio io_context for unrelated coroutines for up to 500ms. Should use `boost::asio::steady_timer` with `co_await async_wait(use_awaitable)`.

**Tertiary problem:** Even on timeout, the planner falls back to `fail_closed` only. Design Section 14.3 specified three fallback modes (`fail_closed`, `native`, `shadow_only`). Only the first exists.

**Impact:** The async execution path is non-functional in any mode. If `QlibExecutionPlanner` is wired up via `SignalEngine::setExecutionPlanner`, **all order placement through it will fail**. Production deployment with this planner active = zero trades.

**Resolution (recommended, design Phase 7 compliant):** Convert to `shadow_only + native fallback`:

```cpp
while (orchestration::sqlite_helpers::nowMs() < deadlineMs) {
    if (planReady(db.get(), requestId, reason)) {
        markRequest(db.get(), requestId, "succeeded", "");
        Logger::instance().log(LogLevel::Info,
            "[QlibExecutionPlanner] plan ready, native fallback for shadow_only request_id=" + requestId);
        // TODO Phase 8: replace with SliceExecutor::execute(planId, draft)
        co_return co_await m_nativeFallback->executeMarket(std::move(draft));
    }
    boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
    t.expires_after(std::chrono::milliseconds(25));
    co_await t.async_wait(boost::asio::use_awaitable);
}
markRequest(db.get(), requestId, "expired", reason);
co_return co_await m_nativeFallback->executeMarket(std::move(draft));  // or fail_closed by config
```

Requires injecting `IExecutionPlanner* m_nativeFallback` via constructor.

---

### 3.2 `run_strategy.py` does not invoke Qlib classes

**Location:** `tools/qlib_bridge/run_strategy.py:210-263`

```python
def run_topk(db, model_id, interval, topk, qlib_class):
    # Fetches latest qlib_predictions
    # SELECT ... ORDER BY score DESC LIMIT topk
    target_weight = 1.0 / max(topk, 1)
    decisions = []
    for symbol, asof_open_time_ms, score, score_percentile, model_run_id in cursor.fetchall():
        decisions.append({...})
    return decisions

def run_strategy_policy(db, args, config, qlib_class):
    if qlib_class.endswith("TopkDropoutStrategy") or qlib_class.endswith("SoftTopkStrategy"):
        topk = int(config.get("topk", config.get("k", 5)))
        return run_topk(db, args.model_id, args.interval, topk, qlib_class)
```

**Problem:** Both `TopkDropoutStrategy` and `SoftTopkStrategy` dispatch to the **same `run_topk` function**. Neither calls into Qlib. `try_load_qlib_class()` (line 46) imports the class but only stores its resolved name in metadata.

**Consequences:**

1. **No `n_drop` rotation logic.** TopkDropout's defining feature — rotating up to `n_drop` symbols per run to control turnover — is missing.
2. **No `trade_impact_limit` for SoftTopk.** SoftTopk's defining feature — capping per-rebalance turnover — is missing.
3. **SoftTopk output is identical to TopK.** Makes the Decision Arbiter's "disagreeing live emits" branch dead code in the only realistic shadow scenario.
4. **Allowlist is decorative.** The class is checked but never used. The allowlist only protects against accidental import — but since import never happens, it protects nothing.

**Impact:** Cannot meaningfully shadow-test two adapters because they always agree. Real strategy comparison is impossible. Promotion decisions would be based on incorrect metrics.

**Resolution (per user direction in review session):** Integrate real Qlib classes.

Steps:

1. After allowlist + import (line 273-274), construct a Qlib `Position` / `Account` view from current trade state (or use a stateless single-tick view).
2. Instantiate the strategy with config params: `TopkDropoutStrategy(topk=k, n_drop=n_drop, signal=qlib_predictions_df, ...)`.
3. Call `strategy.generate_trade_decision()` (Qlib's `TradeDecision` API) for the current trading period.
4. Translate Qlib's `Order` / `TradeDecision` into the project's per-symbol decision rows.
5. For `n_drop`: read previous run's target set from `qlib_strategy_targets`; pass current holdings into Qlib so it can compute rotations.

This is non-trivial work (~80-150 LoC) because Qlib's strategies expect an `executor` and `outer_trade_decision` context that doesn't map cleanly to a stateless run. A thin adapter shim is acceptable.

---

### 3.3 Decision Arbiter has correctness bugs

**Location:** `src/engine/signal_engine.cpp:676-689` and `:720-735`

```cpp
boost::asio::awaitable<void> SignalEngine::processQlibCandidates(...) {
    if (arbiter.candidates.empty()) co_return;

    orchestration::RuntimeStateSnapshot runtimeState;   // ⚠️ default-constructed

    const auto klines = m_scanner.cache().snapshot(symbol, interval);
    if (!klines || klines->empty()) co_return;

    const double currentPrice = klines->back().close;
    const bool globalShadow =
        runtimeState.available &&                       // always false
        (runtimeState.mode == orchestration::ExecutionMode::Shadow ||
         runtimeState.mode == orchestration::ExecutionMode::ShadowOnly);
```

And:

```cpp
int maxPos = std::numeric_limits<int>::max();
double maxRisk = std::numeric_limits<double>::infinity();

auto selectedOpt = arbiter.arbitrate(symbol, maxPos, maxRisk, currentPositions, currentRiskPct, rejected);
```

**Four distinct bugs:**

**(1) `runtimeState` never populated.** `globalShadow` is always `false`. Two cascading consequences:

- `placeOrders = !globalShadow` (line 751) is always `true`. Live orders will be attempted in shadow mode if any non-shadowOnly signal is selected.
- `recordShadow` lambda only records when `globalShadow || cand.signal.shadowOnly`. Real shadow-mode candidates that fail downstream gates never get recorded. Shadow metrics are silently incomplete.

**(2) Aggregate exposure cap inert.** The arbiter API accepts `maxPos` and `maxRisk` arguments but the caller passes `INT_MAX` / `infinity`. The cap check inside `arbitrate()` only fires from per-adapter `cfg.maxConcurrentPositions` and `cfg.maxTotalRiskPct`. Design Section 13.4 specified an aggregate (cross-adapter) cap separate from per-adapter caps. Missing.

**(3) No per-adapter mode filter pre-arbitration.** Design said only adapters in `live` or `live_canary` participate in arbitration; shadow adapters should record shadow signals but not contribute to the consolidated winner. The implementation includes all candidates and filters by per-signal `shadowOnly` flag only — that flag is set inside the plugin and reflects the adapter's mode, but doesn't always correspond exactly to runtime state.

**(4) No structured conflict logging.** Design Section 19 specified `[ARBITER][CONFLICT]` log lines per conflict. The arbiter only appends `(rejected_by_arbiter)` to the signal reason. No structured emit for downstream parsers / alerts.

**Impact:** Shadow metrics are incomplete (bug 1); cross-adapter exposure is unbounded (bug 2); promotion semantics are off (bug 3); audit trail is impoverished (bug 4).

**Resolution (per user direction):** Fix all four in this PR.

```cpp
// 1. Populate runtimeState
orchestration::RuntimeStateSnapshot runtimeState;
if (m_executionStatePort) {
    runtimeState = m_executionStatePort->snapshot();
}

// 2. Wire aggregate caps from config (need new SignalEngine::Config fields)
int maxPos = m_config.qlibArbiter.aggregateMaxConcurrentPositions.value_or(INT_MAX);
double maxRisk = m_config.qlibArbiter.aggregateMaxTotalRiskPct.value_or(
    std::numeric_limits<double>::infinity());

// 3. Pre-filter to live adapters (and record shadow for the rest)
for (auto& cand : arbiter.candidates) {
    if (!isCandidateLive(cand, runtimeState)) {
        recordShadow(cand, "non_live_mode", false, atr, currentPrice);
        // remove from candidates or mark for skip
    }
}

// 4. Structured log on conflict
Logger::instance().log(LogLevel::Info,
    "[ARBITER][CONFLICT] symbol=" + symbol + " winner=" + selected->strategyId +
    " loser=" + cand.strategyId + " winner_dir=" + directionToString(selected->signal.direction) +
    " loser_dir=" + directionToString(cand.signal.direction));
```

---

## 4. Medium Issues

| # | Issue | Location | Suggested fix |
|---|---|---|---|
| M1 | `universe_hash == "default"` silent bypass | `strategy_qlib_strategy_signal.cpp:196, 406`; `run_strategy.py:38` | Either remove the bypass entirely or gate it behind explicit `params.universe_hash_strict = false`. Operators forgetting `--universe-hash` should get loud failure, not silent pass. |
| M2 | Plugin stale defaults wrong for 30m intervals | `strategy_qlib_strategy_signal.cpp:38-39` | Compute defaults at construction from `cfg.intervals[0]`: `maxArtifactAgeSeconds = 2 * intervalSeconds`, `maxDataAgeSeconds = 1.5 * intervalSeconds`. Or require config override and fail validation if absent for non-1h intervals. |
| M3 | Plugin parses caps but doesn't enforce pre-arbiter | `strategy_qlib_strategy_signal.cpp:105-110` | Either remove parsing (caps come from `strategy_config.h` already) or actually enforce. Currently the cap fields appear used but are no-ops in the plugin. |
| M4 | Runtime state encoded as `DecisionRow.reason` prefix string | `strategy_qlib_strategy_signal.cpp:202-275` | Replace with proper struct return from `queryRuntimeState`. The current encoding via string prefix `"runtime_mode="` is fragile and confuses readers. |
| M5 | `qlib_adapter_runtime_state` schema differs from design v1.1 | `run_strategy.py:134-148` | Either update implementation to match design (add `promotion_profile`, `last_decision_at_ms`, etc.) or update design to match implementation. The `state_version` + `active_run_id` pattern is genuinely useful for optimistic concurrency — recommend keeping and updating the design. |
| M6 | `qlib_execution_*` schemas differ from design | `qlib_execution_planner.cpp:81-120`, `run_strategy.py:151-198` | Update design. Implementation's `slice_id UUID PK` is fine; just document the deviation from the composite-PK design. |
| M7 | Watcher hardcodes `slice_count=4`, `duration_ms=60000` | `run_execution_plan_watcher.py:30-31` | Make per-request configurable via `qlib_execution_requests.metadata_json`. Currently every TWAP plan is identical regardless of order size or symbol volatility. |
| M8 | Promotion profiles per `qlib_class` not implemented | (missing) | Either implement per design Section 16 or move to deferred backlog. For MVP, simpler to defer if only one adapter is live at a time. |
| M9 | `processQlibCandidates` → `openPosition` → `m_executionPlanner` chain | `signal_engine.cpp:1240` | Alpha signals also flow through the QlibExecutionPlanner if it's the wired planner. Combined with Critical #1, this means alpha signals fail too. Fix by either (a) guarding the planner to native-only for shadow runs, or (b) fixing Critical #1. |
| M10 | No retry backoff policy in C++ (only `busy_timeout`) | plugin + planner | Acceptable: SQLite's `busy_timeout=5000` effectively retries for 5s. Design's explicit "3 retries with backoff 50ms→200ms→500ms" is a finer-grained policy. Document the decision or implement explicitly. |

---

## 5. Minor / Nitpicks

- **N1.** `try_load_qlib_class()` is dead code path for non-Qlib environments. Either remove or commit to using it for actual instantiation (see Critical #2).
- **N2.** `CLASS_ALIASES` (line 23) is a convenience that drifts from the design's "strict full-path allowlist". Acceptable, but document that aliases are themselves part of the allowlist.
- **N3.** Tests in `test_run_strategy.py` cover only 2 cases: invalid class (allowlist), TopK happy path. Missing: stale data handling, schema version mismatch, transaction rollback on partial write, SoftTopk impact limit, allowlist alias resolution.
- **N4.** `confidence = min(max((score_percentile or 0.0) / 100.0, 0.0), 1.0)` (line 249) assumes `score_percentile` is 0–100. Verify `predict_latest.py` schema; if it writes 0–1 the confidence collapses to 0–0.01.
- **N5.** Plugin's `priority{1000}` default combined with arbiter's `a.cfg.priority < b.cfg.priority` means **lower number wins**. Counterintuitive — most systems use higher-priority-number = wins. Document explicitly, or invert.
- **N6.** Watcher emits no `[QLIB_EXEC][PLAN_READY_LATENCY_MS]` metric (design Section 19 required this).
- **N7.** Logger emits `[QlibExecutionPlanner] fail closed: ...` but no equivalent `[QLIB_EXEC][SLICE_SUBMITTED]` (because there's no slice submission — see Critical #1).
- **N8.** `iexecution_planner.h` exports `ExecutionPlan { planId, isAsync }` but no implementation uses it. Either remove or thread it through `executeMarket` return type.
- **N9.** Plugin includes `winsqlite/winsqlite3.h` fallback (line 8) — verify Windows builds use this correctly; if not, remove for clarity.
- **N10.** `markRequest` (line 198-209) ignores return codes. Failed UPDATE silently leaves request in `pending`. Add error logging at minimum.

---

## 6. Strengths

The implementation deserves credit on several fronts:

- **SQLite contract discipline is correct.** PRAGMA WAL, busy_timeout, foreign_keys, synchronous=NORMAL all set in both runtimes. Schema version negotiation via `PRAGMA user_version` works end-to-end.
- **Atomic ready-flag pattern correctly implemented.** `run_strategy.py:284-362` and `run_execution_plan_watcher.py:126-128` both use single-transaction commits with `status='succeeded'` as the final state update.
- **IExecutionPlanner abstraction lands the Phase 6 reordering correctly.** `NativeExecutionPlanner` is a pass-through; no behavior change for existing strategies. The seam is in place for Phase 7/8 to replace.
- **Decision Arbiter scaffolding is structurally sound.** The `ArbiterCandidate` / `add` / `arbitrate` / `rejected` pattern matches the design intent. The bugs identified in Critical #3 are wiring problems, not architectural problems.
- **v1 action/direction CHECK constraints are correctly tightened** at both the schema and plugin levels.
- **Schema version check at plugin construction** (line 172-175) provides loud, early failure on schema drift — exactly the design intent.
- **Allowlist is correctly hardcoded** as a `frozenset` in source, not config.

---

## 7. Recommended Next Steps

The user confirmed three directional decisions during the review session:

| Direction | User answer | Implication |
|---|---|---|
| QlibExecutionPlanner stub status | Need more analysis | Recommendation provided: `shadow_only + native fallback` for current PR; SliceExecutor deferred to Phase 8 |
| `run_strategy.py` Qlib integration | Integrate real Qlib classes now | Adds Critical #2 to current PR scope (~80-150 LoC) |
| Decision Arbiter bugs | Fix all 4 in this PR | Adds Critical #3 fixes to current PR scope (~30 LoC) |

### 7.1 Punch list for the current PR

| # | Task | Files | Approx LoC |
|---|---|---|---|
| 1 | Convert QlibExecutionPlanner to `shadow_only + native fallback`; replace `sleep_for` with asio steady_timer | `qlib_execution_planner.{h,cpp}` | +50 / -20 |
| 2 | Inject `IExecutionPlanner* nativeFallback` via constructor | callers + ctor | +15 |
| 3 | Populate `runtimeState` in `processQlibCandidates` via `m_executionStatePort->snapshot()` | `signal_engine.cpp:679` | +5 |
| 4 | Wire aggregate exposure caps from config into arbiter call | `signal_engine.cpp:728-731` + Config struct | +15 |
| 5 | Filter per-adapter mode (live / live_canary only participate) before adding to arbiter | `signal_engine.cpp:542-549` | +10 |
| 6 | Add structured `[ARBITER][CONFLICT]` log emission | `signal_engine.cpp:668` | +5 |
| 7 | Integrate real Qlib `TopkDropoutStrategy.generate_trade_decision()` | `run_strategy.py` | +80-150 |
| 8 | Add `n_drop` rotation logic (read previous run's targets, diff) | `run_strategy.py` | +30 |
| 9 | Add `trade_impact_limit` enforcement for SoftTopk | `run_strategy.py` | +20 |
| 10 | Fix plugin stale defaults to be interval-derived (2×, 1.5×) | `strategy_qlib_strategy_signal.cpp:38-39` | +10 |
| 11 | Gate `universe_hash == "default"` bypass behind `dev_mode` config flag | plugin + `run_strategy.py` | +10 |
| 12 | Plugin: enforce `maxConcurrentPositions` / `maxTotalRiskPct` pre-arbiter (defense in depth) | plugin | +25 |
| 13 | Replace plugin's runtime state string-prefix encoding with proper struct | plugin | +20 / -10 |
| 14 | Tests: SoftTopk impact limit, n_drop rotation, stale data, schema mismatch, arbiter conflict scenarios, universe_hash bypass coverage | tests/ | +100-200 |

**Total estimate:** ~400-600 LoC including tests. Reviewable as a single PR.

### 7.2 Deferred to subsequent PRs

| Phase | Scope |
|---|---|
| Phase 7.1 (next PR) | Per-slice metrics emission, watcher slice_count config from `metadata_json`, schema doc reconciliation (update design to match impl deviations) |
| Phase 8 (later) | SliceExecutor: async slice submission loop, per-slice revocation re-query, slice status updates, integration tests with `MockOrdersPort` + time advancement, exchange interaction error handling |
| Phase 9+ | Promotion profiles per `qlib_class`, multi-adapter shadow comparison reports, `ACStrategy` / `SBBStrategyEMA` evaluation, v2 close/short semantics |

---

## 8. Design Document Updates Recommended

Several implementation choices diverge from design v1.1 in ways the design should formally acknowledge:

1. **Section 10.6 + 10.7 (Schema):** Implementation uses `slice_id UUID PK` (not composite `(plan_id, slice_index)`). Implementation uses `state_version` + `active_run_id` for runtime state (not `promotion_profile` + audit columns). Update design to match or change implementation — recommend updating design since the implementation's pattern is sound.

2. **Section 10.0 (Database Connection Contract) retry policy:** Implementation uses SQLite `busy_timeout=5000ms` rather than explicit 3-step backoff. Either accept as equivalent and update the design, or implement the explicit retry. The functional outcome is similar.

3. **Section 11.3 (Plugin config example):** Update to show that `max_artifact_age_seconds` / `max_data_age_seconds` are required (or that defaults are interval-derived). Current example values would still cause production drift.

4. **Section 14.3 (Fallback modes):** Implementation does not support `native` / `shadow_only` fallback modes. Update design to reflect that v1 supports only `fail_closed`, OR update implementation to add the other two.

These would become a `v1.2` design revision once the punch list lands.

---

## 9. Review Decision Log

Decisions made during the review session that shape next-step implementation:

| # | Decision | Alternatives considered | Resolution |
|---|---|---|---|
| 1 | QlibExecutionPlanner direction | (a) Complete SliceExecutor; (b) Shadow_only + native fallback; (c) Remove from flow | **Resolved later.** v1.1 adopted (b) for the shadow-only path; v1.2 completed Phase 8 slice execution. |
| 2 | `run_strategy.py` Qlib integration | (a) Keep score-sort reimpl; (b) Integrate real Qlib; (c) Manual n_drop/impact_limit; (d) Defer SoftTopk | **Chose (b): Integrate real Qlib classes now.** Larger scope but unblocks meaningful multi-adapter shadow runs. |
| 3 | Decision Arbiter bug fixes | (a) Fix all 4 in this PR; (b) Fix bugs 1+4 only; (c) Defer to v1.2 | **Chose (a): Fix all 4 in this PR.** All four are blockers for first meaningful shadow run. |

---

## 10. Acceptance Criteria for Closing the Review

The implementation is acceptance-ready when:

1. ✅ All 13 design decisions verified as implemented (currently 4 ✅, 6 ⚠️, 3 ❌)
2. ✅ Critical issues #1, #2, #3 resolved
3. ✅ Plugin stale defaults are interval-derived
4. ✅ Universe hash bypass gated behind explicit `dev_mode` flag
5. ✅ Runtime state populated correctly in arbiter caller
6. ✅ Aggregate exposure cap wired from config
7. ✅ Per-adapter mode filter applied before arbitration
8. ✅ Structured `[ARBITER][CONFLICT]` log emitted
9. ✅ Real Qlib `TopkDropoutStrategy.generate_trade_decision()` invoked from `run_strategy.py`
10. ✅ `n_drop` rotation logic produces measurable turnover difference vs prior run
11. ✅ SoftTopk `trade_impact_limit` produces measurable difference vs TopK
12. ✅ Test suite covers: SoftTopk impact limit, n_drop rotation, stale data, schema mismatch, arbiter conflict, universe_hash bypass
13. ✅ Design v1.2 written with schema reconciliations + retry policy clarifications + fallback mode update

The current implementation is approximately **40%** complete against design v1.1. The punch list closes the remaining 60% in a single reviewable PR (excluding the Phase 8 SliceExecutor).

---

## 11. Implementation Handoff

Recommended order of work for the next PR:

1. **Plumbing first (items 1-6, ~100 LoC):** Fix Critical #1 (QlibExecutionPlanner shadow_only + native fallback) and Critical #3 (all four arbiter bugs). These are mechanical and unblock real shadow metrics.
2. **Then Qlib integration (items 7-9, ~130-200 LoC):** Critical #2. This is the largest single chunk and likely requires iterative Qlib API exploration. Land it after plumbing is verified.
3. **Then hygiene (items 10-13, ~75 LoC):** Stale defaults, universe_hash bypass, runtime state encoding cleanup, plugin cap enforcement.
4. **Then tests (item 14, ~100-200 LoC):** Once implementation is stable, expand test coverage to the scenarios that would otherwise leak to production.

Reviewer should re-run this verification table after the PR lands. Targeting status: **13 ✅, 0 ⚠️, 0 ❌**.

---

**End of review.**
