# Remediation Handoff — Remaining Work

**Date:** 2026-05-30  
**Branch:** `fix/wr-remediation-2026-05-30`  
**Based on:** `main` @ `f0f4dbc` ("fix some bugs")  
**Review sources:**
- [`2026-05-29-src-v1.0.md`](2026-05-29-src-v1.0.md) — Codex multi-agent, first full-tree review
- [`2026-05-29-src-v1.1.md`](2026-05-29-src-v1.1.md) — Claude parallel-subagent, independent corroborating review

---

## Build / Test

```powershell
# Configure (only needed once)
cmake --preset windows-msvc-debug

# Build tests (NOT the main exe — it may be locked by a running instance)
cmake --build build/windows-msvc-debug --config Debug --target binance_trading_bot_tests -j 4

# Run suite
cd build/windows-msvc-debug
ctest -C Debug -j 4
```

**Test exe:** `build/windows-msvc-debug/bin/Debug/binance_trading_bot_tests.exe`  
**Baseline on this branch:** 470/470 passing (4 network-integration tests skipped by design)

### ⚠️ Tool caveats for this repo
- The `rtk` bash hook intercepts shell commands and can corrupt/fabricate `git`/`grep` output. Always verify edits with the **editor Grep tool** (reliable) rather than bash grep.
- `LNK1168` on `binance_trading_bot.exe` is a file-lock error (running instance), not a compile error. Build only `--target binance_trading_bot_tests` during development.
- Use PowerShell `git` (not bash `git`) for commit/status — bash git output went through the hook.

---

## Branch Commits (What Is DONE)

| Commit | Description | Status |
|--------|-------------|--------|
| `fb4719c` | Prior-session CR/WR fixes (CR-1..CR-39 subset, WR-19,22,39,60,61,74,93,94) | ✅ Verified 470/470 |
| `512be69` | **CR-8**: protection sized to `executedQty` (not requested size or polling) | ✅ + test |
| `d46e84f` | CR-9 design comment; `*Raw` boundary fields on account types; WsClient guard (partial — cpp was still broken) | ⚠️ See note |
| `ebbf1a8` | **WsClient**: complete `CallbackGuard` migration in `.cpp`, closes in-flight UAF | ✅ + 470/470 |

**Note on `d46e84f`:** This commit's message claims "CR-36 free-margin fail-closed" but those edits silently failed to apply (wrong anchor strings) — see **CR-36** below. It does correctly: (a) add a CR-9 design comment, (b) add `*Raw` string fields to `account.h`/`Balance`/`Position`/`FuturesAccount` (boundary preservation only), and (c) introduce the `CallbackGuard` struct in `ws_client.h`.

---

## NOT Done — Named Items Still Outstanding

### CR-36: Consume `*Raw` decimal strings in computations ❌

**What exists:** `*Raw` string fields on `Balance`, `Position`, `FuturesAccount` (boundary preservation). The REST parser populates them. Nothing reads them for decisions.

**What is needed:**  
In `src/account/account_service.cpp`, method `checkFreeMargin(MarginCheckDraft)` (~line 180):

```cpp
// Real API — NOT checkOrderMargin:
//   AccountService::checkFreeMargin(MarginCheckDraft)
//   result: MarginCheckResult { completeness, serverValidated, estimatedRemainingFreeMargin, ... }
//   options: MarginCheckOptions{ useServerTestOrder, useLocalEstimate }

// Local estimate region (~line 195-210):
const double notional = draft.quantity.toDouble() * draft.assumedPrice.value().toDouble();
// ↑ BUG: toDouble() returns 0.0 on parse failure → notional=0 → full balance looks free
```

**Fix:**
1. Parse `availableBalance` from `availableBalanceRaw` (fall back to `double` field only if raw is empty).
2. If `quantity <= 0` or `price <= 0` (including parse failures), return `MarginCheckCompleteness::Unavailable` / not-estimated, NOT a favorable zero-notional estimate.
3. Extend same pattern to `src/engine/sizing_policy.cpp` if it reads `Position` monetary doubles directly.

**Tests:** Add to `tests/test_account_service.cpp` using real API (`MarginCheckDraft`, `checkFreeMargin`, `MarginCheckOptions{.useLocalEstimate=true}`). Lock: bad quantity → estimate unavailable/not-favorable.

---

### WR-71 / WR-22 (v1.1): `+inf` ratios → SQLite NULL → breach flip on restart ❌

**Location:** `src/risk/risk_metrics.cpp`, inside `RiskMetrics::compute()`, ~lines 77-83:

```cpp
out.sortinoRatio = out.stdDevDownside > 0.0
    ? (out.excessReturn / out.stdDevDownside)
    : std::numeric_limits<double>::infinity();   // ← BUG
out.upi = out.ulcerIndex > 0.0
    ? (out.excessReturn / out.ulcerIndex)
    : std::numeric_limits<double>::infinity();   // ← BUG
```

`+inf` serializes as `NULL` in SQLite → reloads as `0.0` → `OK` window becomes `BREACH` after restart (v1.1 WR-22).

**Fix (sign-aware, fail-closed):**
```cpp
// sortinoRatio: zero downside = no demonstrated risk-adjusted edge
out.sortinoRatio = out.stdDevDownside > 0.0
    ? (out.excessReturn / out.stdDevDownside)
    : (out.excessReturn > 0.0 ? 0.0 : out.excessReturn);

// upi: zero drawdown = non-negative return, no edge demonstrated
out.upi = out.ulcerIndex > 0.0
    ? (out.excessReturn / out.ulcerIndex)
    : 0.0;
```

**Also add (WR-22):** guard `sqlite3_bind_double` against non-finite values in `src/risk/risk_db.cpp`; on warm-start, seed `HARD_BREACH` if the last persisted state was a hard breach.

**Existing test to update:** `RiskMetricsTest.UsesInfinityWhenNoDownsideReturnsOrDrawdowns` **asserts the old `+inf` behavior** — it must be updated to the new finite values.

**Add tests:** zero-variance returns → finite sortino ≤ 0; zero-drawdown equity → finite UPI = 0.

---

## Full Remaining Backlog by Module

Severity levels (H = high / M = medium / L = low) are based on live-trading risk.

---

### account/ (v1.0 WR-1..6)

| ID | Severity | File : line | Fix |
|----|----------|-------------|-----|
| WR-1 | M | `account/mql4_account_adapter.cpp:32` | Unknown `multiAssetsMargin` → return `unsupported`, not single-asset defaults |
| WR-2 | M | `account/account_service.cpp:162` | Free-margin test-order draft must mirror real order fields (side, hedge mode) |
| WR-3 | H | `account/account_service.cpp:199` | Local estimate ignores fees, maintenance margin, brackets — mark advisory unless server-validated (overlaps CR-36 above) |
| WR-4 | M | `account/account_service.cpp:224` | `liquidationRisk()` is position-only; add bracket-aware maintenance margin or rename/document scope |
| WR-5 | M | `account/account_service.cpp:79` | AccountService coroutines access `this` + REST after suspension → document lifetime contract or use shared ownership |
| WR-6 (v1.1 IN-13) | M | `account/account_service.cpp:91` | Snapshot makes non-atomic REST calls; partial failures fail everything → offer best-effort partial completion |

---

### backtest/ (v1.0 WR-7..16, v1.1 WR-29..31)

| ID | Severity | File : line | Fix |
|----|----------|-------------|-----|
| WR-7 | M | `backtest/gemini_range_proposer.cpp:352` | Cache key ignores direction/asof → serve stale/opposite-side proposal → include in key |
| WR-8 | H | `backtest/gemini_range_proposer.cpp:601` | Gemini subprocess inherits full bot env (secrets) → sanitized allowlist env |
| WR-9 (v1.1 CR-12b) | M | `backtest/backtest_engine.cpp:207` | Ignores SL/TP hits on entry candle → simulate intrabar exits conservatively |
| WR-10 | M | `backtest/backtest_engine.cpp:141` | Open positions dropped at fold end → force-close or mark-to-market |
| WR-11 (v1.1 CR-12a) | H | `backtest/backtest_engine.cpp:220` | SL tick rounding inverted vs live (Long SL rounds up in backtest, down in live) → shared `quantizeSL()` helper |
| WR-12 | M | `backtest/backtest_gate_controller.cpp:280` | Final stop price not exchange-rounded or validated (tick/price filters, sign) |
| WR-13 (v1.1 WR-29) | M | `backtest/historical_window_provider.cpp:70` + `rest_backfilling_historical_window_provider.cpp:104` | No gap check on historical windows; REST backfill merges without OHLC sanity → validate finite, high≥max(o,c,l), consecutive spacing |
| WR-14 | M | `backtest/gemini_range_proposer.cpp:737` | Parser accepts missing/duplicate tunables → validate exact set, reject dups |
| WR-15 | L | `backtest/gemini_range_proposer.cpp:244` | POSIX duplicate function declaration → remove, add POSIX CI |
| WR-16 | L | `backtest/gemini_range_proposer.cpp:852` | Expired deadline still runs Gemini ~1s → return timeout immediately |
| v1.1 WR-30 | M | `backtest/backtest_engine.cpp:256` | `+inf` Sortino/Sharpe poisons combo mean → cap at finite sentinel or exclude |
| v1.1 WR-31 | M | `backtest/backtest_engine.cpp:70` | `roundToTick` passes non-positive TP prices → reject trade when derived SL/TP ≤ 0 |

---

### catalog/ (v1.0 WR-17,18,20,21,23,24; v1.1 WR-38,39)

| ID | Severity | File : line | Fix |
|----|----------|-------------|-----|
| WR-17 | H | `catalog/strategy_catalog.cpp:20` | Registry mutation unsynchronized at runtime reload → reload startup-only or synchronized snapshot |
| WR-18 | H | `catalog/strategy_catalog.cpp:54` | Fail-open on partial registration → add fail-closed policy (zero strategies = abort) |
| WR-20 | M | `catalog/strategy_catalog.cpp:59` | `raw release()` before `shared_ptr` construction → keep RAII guard through construction |
| WR-21 | M | `catalog/plugin_handle.cpp:76` | Plugin metadata exports can throw/return garbage across ABI → wrap in try/catch, validate strings, require ABI version check |
| WR-23 | M | `common/expected_compat.h:93` | Fallback `variant<T,E>` breaks when `T==E` → discriminate on explicit tag, `static_assert(!is_same_v<T,E>)` |
| WR-24 | M | `common/expected_compat.h:13` | Polyfill injects into `namespace std` (UB) → move to project namespace with `using` aliases |
| v1.1 WR-38 | M | Same files | `expected_compat` polyfill missing monadic API (`value_or`, `and_then`, rvalue `error()`) |
| v1.1 WR-39 | H | `catalog/plugin_loader.cpp:195` | SHA-256 hash→load TOCTOU window still open → hold file with deny-write share across hash+load, or hash the mapped image |

---

### engine/ (v1.0 WR-25..38; v1.1 CR-4, WR-2,4)

| ID | Severity | File : line | Fix |
|----|----------|-------------|-----|
| WR-25 | H | `engine/position_tracker.cpp:19` | Restart: recovered positions lack SL binding → query open algo orders or place emergency SL, or force-close |
| WR-26 | M | `engine/take_profit_reconciler.cpp:481` | `EnforceGlobal` TP mode ignored → implement mode-specific cancel/replace or coverage checks |
| WR-27 | H | `engine/signal_engine.cpp:2301` | Stop replacement: old stop orphaned on cancel failure → treat cancel failure as reconcile-required before tracker update |
| WR-28 | M | `engine/signal_engine.cpp:2342` | Risk analytics misses user-data exit fills → notify risk on successful exit removal |
| WR-29 (v1.1 WR-1) | H | `engine/signal_engine.cpp:2167` | Time-exit close qty ignores step size → floor to `quantityToStepDecimal`; scientific-notation decimal path too |
| WR-30 (v1.1 CR-9) | H | `engine/sizing_policy.cpp:19` | Min-notional clamp can exceed max-notional/risk budget → after ceil bump, check `quantity*price > maxNotional` and reject |
| WR-31 | M | `engine/signal_engine.cpp:1349` | `minQty`/`maxQty` filters not enforced in preflight → include in preflight checks |
| WR-32 | M | `engine/loss_manager.cpp:160` | DCA pending timeout never clears → reconcile/cancel timed-out DCA, transition state |
| WR-33 | H | `engine/loss_manager.cpp:534` | ROI can trigger on missing/zero mark price → require `isfinite(markPrice) && markPrice > 0` |
| WR-34 | M | `engine/qlib_execution_planner.cpp:68` | Qlib metadata JSON hand-concatenated → serialize via nlohmann/json |
| WR-35 | M | `engine/qlib_execution_planner.cpp:372` | Qlib request marked succeeded before slices execute → mark `running/submitting` until all slices accepted |
| WR-36 | M | `engine/gemini_filter.cpp:525` | Gemini subprocess inherits handles/FDs → restrict handle inheritance, close unrelated FDs |
| WR-37 | L | `engine/signal_engine.cpp:759` | Per-strategy scan intervals collapse to global min → track next-due time per strategy/symbol/interval |
| WR-38 | M | `engine/exposure_controller.cpp:81` | Missing beta falls back permissively → block in closed mode or use conservative high beta |
| **v1.1 CR-4** | **H** | `engine/signal_engine.cpp:598,1338,1496` | **SignalEngine shared state raced on 2-thread pool (no strand): `m_lastOpenDecision`, gemini/backtest cycle gates/counters can be read/written concurrently → bind engine coroutines to a single `asio::strand` or protect gates with mutex + atomics** |
| v1.1 WR-2 | H | `engine/qlib_execution_planner.cpp:346,84` | Blocking SQLite (5s busy_timeout) on io thread → offload to dedicated thread pool, post results back |
| v1.1 WR-4 | M | `engine/exposure_controller.cpp:191` | Hard exposure limit bypassed for "deviation-improving" trades → enforce ceiling even when reducing deviation |

---

### orchestration/ (v1.0 WR-40..50; v1.1 WR-25..28)

| ID | Severity | File : line | Fix |
|----|----------|-------------|-----|
| WR-40 (v1.1 WR-27) | M | `orchestration/batch_scheduler_thread.cpp:302` | Daily batch failure suppresses same-day retry; DST shift can skip/duplicate retrain → mark date done only on success; anchor to UTC |
| WR-41 (v1.1 WR-26) | M | `orchestration/process_manager.cpp:184` | Global mutex serializes long subprocess work + sleeps → lock only shared state, run/wait outside mutex |
| WR-42 | M | `orchestration/process_manager.cpp:569` | POSIX daemon stop doesn't kill process tree → `setpgrp()`/`killpg()` |
| WR-43 | H | `orchestration/process_manager.cpp:257` | Subprocesses inherit full secret env → sanitized allowlist environment |
| WR-44 | M | `orchestration/process_manager.cpp:332` | Log files world-readable (`0644`) → create with `0600`, restrict dirs, redact output |
| WR-45 | H | `orchestration/model_publisher.cpp:184` | Publish paths allow traversal from unsanitized `runId`/`modelId` → validate safe IDs, canonical containment |
| WR-46 (v1.1 WR-25) | H | `orchestration/promotion_checker.cpp:91` + `candle_scheduler_thread.cpp:75` | Promotion step errors ignored; scheduler thread terminates on `SQLITE_BUSY` → `require SQLITE_DONE`, wrap `processCandle`/`evaluate` in try/catch within `run()` |
| WR-47 (v1.1 WR-24) | H | `orchestration/promotion_checker.cpp:116` | Sharpe annualization ignores horizon; `minMeanNetReturnBps` defaults `0.0` lets break-even models live → `barsPerYear/horizonBars` annualization; require strictly positive margin |
| WR-48 | M | `orchestration/shadow_metrics_recorder.cpp:536` | Shadow outcome backfill crosses models → filter by model/adapter, persist cost model version |
| WR-49 | M | `orchestration/orchestrator_config.h:123` | Negative cost inputs inflate net returns → validate finite and nonnegative |
| WR-50 | M | `orchestration/qlib_state_store.cpp:85` | Runtime state schema rejects `shadow_only` execution mode → add to CHECK constraint or reject before global write |
| v1.1 WR-28 | M | `orchestration/process_manager.cpp:537` | `isDaemonRunning` treats `WAIT_FAILED` as exited → check `GetExitCodeProcess`/`STILL_ACTIVE`; erase only on confirmed termination |

---

### orders/ (v1.0 WR-51..58; v1.1 WR-7..11,14)

| ID | Severity | File : line | Fix |
|----|----------|-------------|-----|
| WR-51 | H | `orders/orders.cpp:4` | Separate durable journals can share one file path with different mutexes → single shared `DurableOrderJournal` per process |
| WR-52 (v1.1 WR-9) | H | `orders/order_journal.cpp:306` | Corrupt journal line throws out of `loadFromFile` → ctor fails → service won't start; wrap per-line parsing in try/catch, quarantine bad records |
| WR-53 (v1.1 WR-10) | H | `orders/order_validator.cpp:102` | Timestamp override also permits caller-provided signature; `validateRawParams` not called for closeByMarket/stopEntry/protection → always block `signature`; route all draft raw params through allowlist |
| WR-54 | M | `orders/order_journal.cpp:135` | Durable journal world-readable (`0644`) → create with `0600` / restricted ACLs |
| WR-55 | M | `orders/order_journal.cpp:104` | Synchronous `fsync` on order placement path → dedicated journal writer thread with bounded durability policy |
| WR-56 | H | `orders/order_validator.cpp:124` | Exchange filters skipped; orders proceed even when filter data stale/absent → inject exchange-info snapshot; fail when stale |
| WR-57 | M | `orders/normal_order_service.cpp:480` | Amend path lacks validation and journaling → add amend validator, journal intent + result |
| WR-58 | H | `orders/normal_order_service.cpp:476` | Leverage changes have no guardrails → validate range and configured max leverage |
| v1.1 WR-7 | **H** | `orders/order_journal.h:37` + `normal_order_service.cpp:375` | **`pendingReconcile()` is never consumed — orphaned live orders after ambiguous placements go undetected. Add a startup reconciler or confirm an external one exists. Most operationally important item in this module.** |
| v1.1 WR-8 | H | `orders/order_journal.cpp:215` | `DurableOrderJournal` ctor swallows path failure (probe `ofstream` result ignored) → service thinks it's durable but can't persist → hard-fail the ctor |
| v1.1 WR-11 | M | `orders/normal_order_service.cpp:680` | Fill-summary conflates entry/exit VWAP; partial PnL reported as concrete → partition by side; suppress `realizedPnl` on any parse failure |
| v1.1 WR-14 | M | `rest/rate_limiter.cpp:93` | Reservations never released on failure → self-throttling within window → decrement on request completion |

---

### rest/ (v1.0 WR-59..66; v1.1 WR-13,16)

| ID | Severity | File : line | Fix |
|----|----------|-------------|-----|
| WR-59 | M | `rest/rest_client.cpp:1013` | Algo cancel/query sends undocumented `symbol` param → remove or prove API acceptance |
| WR-60 | ✅ | Already fixed in prior session | — |
| WR-61 | ✅ | Already fixed in prior session | — |
| WR-62 | M | `rest/rest_client.h:96` | Public `rawParse` returns a dangling `string_view` to callers outside this session — **verify that the `RawParsedDocument` change (CR-18, prior session) fully resolves this for all callers** |
| WR-63 (v1.1 CR-2) | **H** | `rest/signer.h:19` | **API secrets stored in plain `std::string`, never zeroed, exist in ≥3 heap locations → `OPENSSL_cleanse`-on-destroy buffer, minimize copies, zeroize in destructors** |
| WR-64 | M | `rest/signer.cpp:77` | Caller-provided signature bypasses signing → verify `removeQueryParam("signature")` + forced recompute covers all paths |
| WR-65 | M | `rest/rest_client.cpp:600` | High-weight reads (depth, open-orders) treated as cost 1 → per-endpoint variable weights |
| WR-66 | M | `rest/rest_client.cpp:254` | Financial values parsed into `double` → preserve decimal strings or fixed-point |
| v1.1 WR-13 | M | `rest/rest_client.cpp:344` | `isClosed` derived from local wall-clock vs server `closeTime` → derive from interval/openTime or server-time reference |
| v1.1 WR-16 | M | `rest/rate_limit_headers.cpp:23` | Header parser rejects trailing whitespace/`\r` (proxies don't trim) → trim before `from_chars`; parse to `long long`, clamp |

---

### risk/ (v1.0 WR-67..72; v1.1 WR-22,23)

| ID | Severity | File : line | Fix |
|----|----------|-------------|-----|
| **WR-71** | **H** | `risk/risk_metrics.cpp:77-83` | **`+inf` sortino/UPI — see detailed fix section at top of this document** |
| WR-67 | M | `risk/risk_controller.cpp:146` | Equity sample write failure terminates engine coroutine instead of applying failure policy → catch and apply mode |
| WR-68 | M | `risk/risk_controller.cpp:221` | Exact threshold hits (boundary drawdown/UPI) not treated as breaches → use `>=` / `<=` inclusive |
| WR-69 | M | `risk/risk_controller.cpp:113` | Public constructor bypasses `fromJson()` validation → validate in constructor, require finite thresholds |
| WR-70 | M | `risk/risk_controller.cpp:160` | Concurrent recomputes race past interval gate → in-flight gate or strand |
| WR-72 (v1.1 WR-23) | M | `risk/risk_controller.cpp:168,187` | Recompute blocks Asio executor; retention `DELETE` on hot path → offload to worker executor; wrap recompute + prune in one transaction at separate cadence |
| v1.1 WR-22 | H | `risk/risk_db.cpp` + `risk_controller.cpp` | Warm-start: `NULL` → `0.0` flips breach status → guard `bind_double` against non-finite; seed `HARD_BREACH` on warm-start if last state was hard breach |

---

### scanner/strategy/transport (v1.0 WR-73,75,76,78..86)

| ID | Severity | File : line | Fix |
|----|----------|-------------|-----|
| WR-73 | M | `scanner/kline_cache.cpp:93` | REST backfill can overwrite fresher WS candle → add freshness rules, skip current forming REST candle |
| WR-75 | M | `scanner/market_scanner.cpp:520` | Stale cache symbols survive restart → clear cache or intersect with current symbol info |
| WR-76 | M | `scanner/market_scanner.cpp:457` | Kline request limit not capped to 1500 → clamp + paginate |
| WR-78 | M | `strategy/indicators/atr.cpp:16` | ATR propagates malformed OHLC (NaN/inf/invalid) → validate candle fields, return error/optional |
| WR-79 | H | `strategy/strategy_registry.cpp:25` | Registry returns raw strategy pointers; reload/clear can produce dangling work items → return shared immutable snapshots |
| WR-80 | H | `strategy/strategy_registry.cpp:11` | Registry mutations unsynchronized on hot reload/shutdown → `shared_mutex` or immutable startup-only registry |
| WR-81 | M | `strategy/istrategy.h:32` | Plugin ABI exposes C++ STL/virtual (compiler/CRT mismatch crashes) → versioned C ABI or enforce exact ABI compatibility |
| WR-82 | ✅ | Prior session | (Verify: zero/negative timing config clamped) |
| WR-83 (v1.1 WR-17) | M | `transport/socks5_proxy.h:37,76` | SOCKS5 handshake can hang; bound-address length untrusted → deadline across tunnel handshake; bound length sanity check |
| WR-84 | M | `transport/ws_session.cpp:176` | WS reconnect timer depends on message traffic → independent reconnect timer |
| WR-85 | M | `transport/ws_session.cpp:171` | WS callback receives ephemeral `string_view` → pass owned `string` or enforce synchronous contract |
| WR-86 | M | `transport/http_session.cpp:215` | HTTP failure classification too coarse (write vs post-send) → split classification, reconcile post-send orders |
| v1.1 CR-5 | **H** | `ws/ws_client.cpp`, `ws/user_data_stream.cpp`, `transport/ws_session.cpp` | **WS stack data races: `m_subscriptions` iterated on io threads while mutated from caller threads (outside `m_parserMutex`); `m_listenKey`/`m_session` torn reads. The CallbackGuard fix (this branch) closes the UAF; but the subscription-map iteration race and listen-key torn-read remain. Bind all WS object operations to strands.** |
| v1.1 CR-6 | H | `ws/ws_client.cpp:207`, `scanner/market_scanner.cpp:514` | Self-recreating reconnect session: reconnect lambda calls `connect()` which `stop()`s the running session from inside its own coroutine. Use WsSession's built-in reconnect; `onReconnect` should only notify. |

---

### types / ws / root + Info items (v1.0 WR-87..98, IN-1..8; v1.1 CR-11, WR-35..37, IN-1..16)

| ID | Severity | File : line | Fix |
|----|----------|-------------|-----|
| **v1.1 CR-11** | **H** | `types/events.h:104-117` | **`OrderUpdateEvent`/`LiquidationEvent` use `double` for `avgPrice`, `lastFilledPrice/Qty`, `realizedPnl`, `commission` while REST types keep exact `std::string` → unify to string/fixed-point; convert numeric at last moment only. Highest accounting-integrity risk.** |
| WR-87 | M | `types/market.h:74` | Exchange filters collapse distinct constraints (market-lot-size, notional, price) → model each filter type explicitly |
| WR-88 | M | `types/trade.h:46` | Raw order params bypass typed safety → validate `extraParams` at REST boundary |
| WR-89 (v1.1 IN-11) | M | `types/trade.h:29` | Default `OrderRequest` is actionable BUY MARKET; `OrderSide` defaults `Buy` → require explicit side or return error on unset |
| WR-90 | M | `types/events.h:120` | Order-update field misnames trade ID as time → rename to `tradeId`, add timestamp fields |
| WR-91 | L | `types/events.h:38` | Book ticker lacks update sequencing → add update IDs/timestamps, enforce monotonic |
| WR-92 | L | `types/account.h:88` | Leverage bracket metadata incomplete → add missing bracket fields |
| WR-95 (v1.1 WR-18) | M | `ws/ws_parse_helpers.h:37` | Numeric zero fallback for malformed fields; `intField` lacks string fallback (unlike `doubleField`) → give `intField` same string fallback; return error/optional for zero |
| WR-96 | M | `trading_engine.cpp:321` | TradingEngine callbacks unsynchronized → register before `start()` or protect with mutex/atomic |
| WR-97 | M | `trading_engine.cpp:68` | Indicator periods not validated → validate positive before start |
| WR-98 | M | `binance_api.cpp:39` | Legacy wrapper blocks on futures → can deadlock if called on its own executor; expose awaitable or assert off-executor |
| v1.1 WR-35 | M | `main.cpp:77` | Signal handler calls `Logger::log()` (not async-signal-safe) → only `g_running = false` in handler; log after |
| v1.1 WR-36 | H | `main.cpp:509` | Malformed `config.json` silently treated as empty → parse error is fatal (not fallback to defaults) |
| v1.1 WR-37 | L | `logger.cpp:46` | `std::localtime` not thread-safe → `localtime_r`/`localtime_s` |
| IN-1 | L | `catalog/plugin_loader.cpp:276` | Windows `.Dll` case-insensitive matching → lowercase before compare |
| IN-2 | L | `rest/rest_client.cpp:1260` | Listen-key endpoints bypass rate limiter → route through helper |
| IN-3 | L | `risk/risk_db.cpp:248` | Retention full-scan `equity_points` → add timestamp index, cache retention |
| IN-4 | L | `scanner/market_scanner.h:32` | Unused throttling knobs (pacing/concurrency) → implement or remove |
| IN-5 | L | `scanner/market_scanner.cpp:119` | Public scanner clients copy API secrets → split public transport config |
| IN-6 | L | `strategy/indicators/atr.cpp:10` | ATR period overflow before cast → bounds-check, use `size_t` |
| IN-7 | L | `strategy/indicators/atr.cpp:37` | `lastAtr()` allocates full vector → O(1) streaming last-ATR path |
| IN-8 | L | `types/market.h:20` | Duplicated kline aliases (`quoteVolume`/`quoteAssetVolume`) → canonical field + accessor |
| v1.1 IN-1 | L | `ws/ws_client.cpp:233` + `ws/user_data_stream.cpp:235` | Hot-path allocs per WS message (padded_string + parser per frame) → reuse member buffer/parser |
| v1.1 IN-2 | L | `engine/exposure_controller.h:23` | `ExposureConfig` has no validator unlike `LossManagerConfig` |
| v1.1 IN-5 | L | `orders/order_id_generator.cpp:47` | `clientOrderId` charset allows `/`/`:`, margin tight when epoch ms hits 14 digits |
| v1.1 IN-6 | L | `engine/take_profit_reconciler.cpp:228` | TP id fingerprint narrow → widen to ~4 base36 chars |
| v1.1 IN-7 | L | `rest/rate_limiter.cpp:163` | `isNearLimit()` basis mismatch + 1ms spin → track real budget; sleep computed window remainder |
| v1.1 IN-8 | L | `ws/ws_client.cpp:122` | `parseMarketEvent` falls through to `CompositeIndexEvent` for unknown payloads → explicit unknown/error variant |
| v1.1 IN-12 | L | `types/market.h:14-22` | `tradeCount`/`numberOfTrades` duplicates; global-namespace `Trade` collision → collapse aliases, namespace types |

---

## Suggested Execution Order

1. **WR-71 + WR-22** — risk metrics `+inf` and SQLite NULL→0.0 (small, high-impact, isolated)
2. **CR-36** — free-margin fail-closed (small, account_service.cpp, exact API above)
3. **v1.1 CR-11** — `OrderUpdateEvent` money typing to string (accounting integrity)
4. **v1.1 CR-4** — `SignalEngine` strand (engine race, critical for live concurrency)
5. **v1.1 CR-5/CR-6** — WS subscription-map race and reconnect self-recreation
6. **WR-63** — API secret zeroization
7. **WR-25, WR-27, WR-30, WR-31** — position tracker / sizing / orphan stop
8. **v1.1 WR-7 (orders)** — `pendingReconcile` reconciler (most important orders item)
9. **WR-56, WR-58** — exchange filter enforcement, leverage guardrails
10. Then remaining module batches: backtest, orchestration, catalog, rest, types, IN items

## Per-Session Checklist

```
[ ] git status (PowerShell) clean before starting
[ ] Edit code + tests (one module per session)
[ ] Verify edits landed with editor Grep tool (not bash grep — rtk hook unreliable)
[ ] cmake --build build/windows-msvc-debug --config Debug --target binance_trading_bot_tests -j 4
[ ] cd build/windows-msvc-debug && ctest -C Debug  →  470+/all passing
[ ] PowerShell git commit with Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
[ ] Update remediation progress in docs/comprehensive-reviews/2026-05-29-src-v1.0.md
```
