# ✅ Zip Strategy Symbol Work Queue

**Version:** 1.0
**Date:** 2026-05-16
**Status:** ✅ DONE - Implemented

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-05-16 | Initial design for zip-based strategy/symbol scheduling in `SignalEngine` work queue |

---

## 1. Muc Tieu

Thay doi thu tu tao work queue trong `SignalEngine` de moi scan cycle phan bo co hoi danh gia signal theo cap `strategy + symbol`, thay vi de strategy dau tien quet het tat ca symbols truoc.

**Thu tu hien tai:**

```text
strategy[0] / symbol[0] / interval[0]
strategy[0] / symbol[0] / interval[1]
strategy[0] / symbol[1] / interval[0]
strategy[0] / symbol[1] / interval[1]
strategy[1] / symbol[0] / interval[0]
...
```

**Thu tu mong muon:**

```text
strategy[0] / symbol[0] / all strategy[0] intervals
strategy[1] / symbol[1] / all strategy[1] intervals
strategy[0] / symbol[2] / all strategy[0] intervals
strategy[1] / symbol[3] / all strategy[1] intervals
...
```

Voi config hien tai, vi co 2 strategy active:

```text
Trend Breakout Trader / symbol A / 30m
Trend Breakout Trader / symbol A / 1h
Trend Breakout Trader / symbol A / 4h
Gartley 3&6 Candle Crossover / symbol B / 1d
Gartley 3&6 Candle Crossover / symbol B / 4h
Gartley 3&6 Candle Crossover / symbol B / 1h
Gartley 3&6 Candle Crossover / symbol B / 30m
Trend Breakout Trader / symbol C / 30m
...
```

**Non-goals:**

- Khong thay doi logic `evaluate()` cua bat ky strategy nao.
- Khong thay doi sizing, order placement, exposure control, trailing stop, hay Gemini filter policy.
- Khong auto-enable strategy khong co trong `config.json`.
- Khong thay doi `config.json` trong design nay.

---

## 2. Hien Trang

`main.cpp` doc `config.json`, giu nguyen thu tu mang `strategies`, sau do `StrategyCatalog::initialize()` tao strategy instance va add vao `StrategyRegistry`.

`SignalEngine::runScanCycle()` lay symbols tu scanner, goi `WorkQueue::build(symbols, registry)`, roi xu ly tung `WorkItem` tuan tu bang `processItem()`.

Hien tai `WorkQueue::build()` lap theo thu tu:

```text
for strategy in registry.all():
  for symbol in symbols:
    for interval in strategy.config().intervals:
      enqueue(strategy, symbol, interval)
```

He qua:

- Strategy dau tien trong config co co hoi tao candidate signal truoc tren toan bo symbol universe.
- Khi `gemini_filter.max_evaluations_per_scan_cycle` thap, cac strategy sau co the gap budget exhausted truoc khi duoc danh gia candidate.
- Neu symbol order khong deterministic, viec debug runtime order kho hon.

---

## 3. Understanding Summary

- Can thiet ke lai thu tu goi strategy trong scan cycle.
- Rule da xac nhan: zip `strategy + symbol`.
- Voi moi cap `strategy[i] + symbol[i]`, engine chay toan bo intervals cua strategy do.
- Neu het strategy nhung con symbols, strategy index quay vong theo modulo.
- Mapping strategy-symbol la static trong mot cycle va giu nguyen giua cac cycle neu symbol set khong doi. Cross-cycle rotation khong nam trong scope cua design nay.
- Thu tu strategy van den tu `config.json`.
- Thu tu interval van den tu config cua strategy.
- Thay doi nen nam o tang work queue de khong anh huong logic trading con lai.

---

## 4. Assumptions Va Constraints

### 4.1 Assumptions

| # | Assumption |
|---|---|
| A1 | Strategy order la thu tu `strategies` trong `config.json`. |
| A2 | Neu `symbols.size() > strategies.size()`, dung modulo: `strategyIndex = symbolIndex % strategies.size()`. |
| A3 | Neu `strategies.size() > symbols.size()`, chi tao work item cho cac symbols hien co; strategy khong duoc pair trong cycle do se khong chay. |
| A4 | Moi cap strategy/symbol enqueue tat ca intervals cua strategy truoc khi sang cap tiep theo. |
| A5 | Queue order can deterministic de logs va tests de doc. |
| A6 | `KlineCache::symbols()` hien dung `unordered_map`, nen implementation nen sort symbols trong `WorkQueue::build()` hoac truoc khi build queue. |
| A7 | Mapping strategy-symbol la static; khong co offset/rotation qua cac scan cycle trong version 1.0. |

### 4.2 Non-Functional Requirements

| Category | Requirement |
|---|---|
| Performance | Build queue la O(symbols * average intervals), khong tao nested cross-product strategy x symbol nua. |
| Scale | Ho tro hang tram symbols va nhieu strategies ma khong tang bo nho bat thuong. |
| Reliability | Queue order phai on dinh giua cac scan cycle neu active symbol set khong doi. |
| Maintainability | Thay doi tap trung o `WorkQueue::build()` va tests lien quan. |
| Security/Privacy | Khong them external calls, khong thay doi credential flow, khong log secret. |
| Observability | Runtime logs hien co van duoc giu; co the them unit test order thay vi them log moi. |

---

## 5. Design Approaches Considered

### Option A: Zip Strategy + Symbol In `WorkQueue::build()` (Recommended)

`WorkQueue::build()` lay `registry.all()`, sort symbols, sau do lap theo symbol index:

```text
for i in 0..symbols.size()-1:
  strategy = strategies[i % strategies.size()]
  symbol = sortedSymbols[i]
  for interval in strategy.config().intervals:
    enqueue(strategy, symbol, interval)
```

Pros:

- Dung chinh xac rule da xac nhan.
- Giam so work items so voi cross-product hien tai.
- Scope nho, de test.
- Khong can config moi.

Cons:

- Moi symbol chi duoc gan voi mot strategy trong mot scan cycle.
- Neu co 2 strategy, `symbol A` chi duoc strategy 1 danh gia trong cycle do, khong duoc strategy 2 danh gia cung cycle.
- Neu `strategies.size() > symbols.size()`, mot so strategy se khong chay trong cycle do. Day la intentional trade-off cua zip scheduling, khong phai exhaustive coverage.

Business risk:

- This is a semantic breaking change from strategy-major exhaustive coverage. A strategy that would have produced a valid signal for a symbol can be completely skipped for that symbol while the symbol set and strategy order stay stable.
- Example: with `strategies=[TrendBreakout, Gartley]` and sorted symbols `[BTCUSDT, ETHUSDT]`, `BTCUSDT` is evaluated only by `TrendBreakout` and `ETHUSDT` only by `Gartley` every cycle until the symbol set or scheduler policy changes.
- Operators must treat `zip_strategy_symbol` as reduced signal coverage, not just a queue-order optimization.
- If exhaustive per-strategy/per-symbol coverage is required for production trading, use a future `strategy_major` or `symbol_major` policy instead of this v1 zip scheduler.

### Option B: Symbol-Major Round Robin Strategy

Lap symbol truoc, sau do strategy:

```text
for symbol in symbols:
  for strategy in strategies:
    for interval in strategy.intervals:
      enqueue(...)
```

Pros:

- Moi symbol duoc moi strategy danh gia trong cung scan cycle.
- Cong bang hon theo symbol.

Cons:

- Khong phai rule da xac nhan.
- Van tao full cross-product, Gemini budget van co the bi dung nhanh.

### Option C: Configurable Scheduler Policy

Them config `work_queue_policy = strategy_major | symbol_major | zip_strategy_symbol`.

Pros:

- Linh hoat cho tuong lai.

Cons:

- Tang surface area va test matrix.
- Chua can thiet vi yeu cau hien tai da ro.

**Decision:** Chon Option A.

---

## 6. Target Flow

### 6.1 Startup Flow

Khong thay doi startup flow:

```text
main()
  read config.json
  StrategyCatalog::initialize(strategyConfigs)
  MarketScanner::start()
  SignalEngine::run()
```

### 6.2 Scan Cycle Flow

Target flow:

```text
SignalEngine::runScanCycle()
  symbols = scanner.symbols()
  queue = WorkQueue::build(symbols, registry)
  for item in queue:
    processItem(item)
```

`WorkQueue::build()` target behavior:

```text
strategies = registry.all()
symbols = sorted copy of scanner symbols

if strategies empty or symbols empty:
  return empty queue

for symbolIndex in range(symbols.size()):
  strategy = strategies[symbolIndex % strategies.size()]
  symbol = symbols[symbolIndex]

  if strategy is null:
    continue

  for interval in strategy.config().intervals:
    enqueue WorkItem(symbol, interval, strategy)
```

### 6.3 Example

Input:

```text
strategies = [TrendBreakout, Gartley]
symbols = [BTCUSDT, ETHUSDT, SOLUSDT, BNBUSDT]
```

Output:

```text
TrendBreakout / BTCUSDT / 30m
TrendBreakout / BTCUSDT / 1h
TrendBreakout / BTCUSDT / 4h
Gartley / ETHUSDT / 1d
Gartley / ETHUSDT / 4h
Gartley / ETHUSDT / 1h
Gartley / ETHUSDT / 30m
TrendBreakout / SOLUSDT / 30m
TrendBreakout / SOLUSDT / 1h
TrendBreakout / SOLUSDT / 4h
Gartley / BNBUSDT / 1d
Gartley / BNBUSDT / 4h
Gartley / BNBUSDT / 1h
Gartley / BNBUSDT / 30m
```

---

## 7. Edge Cases

| Case | Expected Behavior |
|---|---|
| No strategies | Queue empty; `SignalEngine` waits default empty-registry interval. |
| No symbols | Queue empty; no strategy evaluation. |
| Null strategy pointer | Skip. |
| Strategy with no intervals | No work items for that strategy/symbol pair. |
| 1 strategy, N symbols | Moi symbol duoc pair voi strategy duy nhat; behavior gan voi cross-product vi chi co mot strategy. |
| More symbols than strategies | Strategy assignment wraps with modulo. |
| More strategies than symbols | Only strategies paired to available symbols run in that cycle. |
| 1 symbol, N strategies | Only `strategies[0]` runs for that symbol in the cycle; other strategies are skipped. |
| `strategies.size() == symbols.size()` | Perfect 1-1 mapping; no modulo wrap is observed. |
| Symbol order from cache is unstable | Sort symbols before queue generation. |
| Plugin load failure | Existing catalog behavior remains: failed strategy is not registered. |

---

## 8. Testing Strategy

### 8.1 Unit Tests

Update `tests/test_work_queue.cpp`:

- `BuildsZipStrategySymbolOrder`
  - strategies: `a` intervals `["1h", "4h"]`, `b` intervals `["30m"]`
  - symbols: `["BTCUSDT", "ETHUSDT", "SOLUSDT"]`
  - expected:

```text
a/BTCUSDT/1h
a/BTCUSDT/4h
b/ETHUSDT/30m
a/SOLUSDT/1h
a/SOLUSDT/4h
```

- `SortsSymbolsForDeterministicZipOrder`
  - input symbols intentionally unsorted
  - expected queue uses sorted symbols.

- `DegeneratesToSingleStrategyAcrossAllSymbols`
  - one strategy, multiple symbols
  - expected every symbol is paired with that one strategy.

- `SkipsUnpairedStrategiesWhenStrategiesExceedSymbols`
  - multiple strategies, one symbol
  - expected only first paired strategy creates work items; no empty/null work items are enqueued.

- `BuildsOneToOneMappingWhenCountsMatch`
  - equal strategy and symbol counts
  - expected each strategy is paired to the symbol at the same sorted index.

- `ReturnsEmptyWhenNoStrategies`

- `ReturnsEmptyWhenNoSymbols`

### 8.2 Regression Tests

Run:

```powershell
ctest --test-dir build --output-on-failure -R "WorkQueue|SignalEngine|Strategy"
```

If local build directory differs, use the repo's existing CMake preset.

---

## 9. Implementation Handoff

Expected files:

| File | Change |
|---|---|
| `src/engine/work_queue.cpp` | Replace strategy-major nested loop with zip strategy/symbol scheduler. |
| `tests/test_work_queue.cpp` | Update existing order test and add deterministic/edge case tests. |

Implementation notes:

- Use `std::sort` on a local copy of `symbols`.
- Keep `registry.all()` as source of strategy order.
- Do not mutate registry, scanner, or strategy configs.
- Keep behavior isolated from `SignalEngine::processItem()`.

---

## 10. Decision Log

| Decision | Alternatives | Rationale |
|---|---|---|
| Use zip `strategy + symbol` scheduling | Strategy-major, symbol-major | Matches confirmed user intent and reduces early dominance by first strategy. |
| Strategy wraps by modulo when symbols exceed strategies | Stop after `min(symbols, strategies)`, full cross-product | Keeps all symbols covered in each cycle while preserving zip rule. |
| Strategy starvation when strategies exceed symbols | Accept, add explicit risk documentation | Zip rule prioritizes paired symbol coverage, not exhaustive strategy coverage. Operators must know that if symbols are fewer than strategies, not every strategy runs each cycle. |
| Warn when strategies exceed symbols | Silent skip | Emits one scan-cycle warning so operators know some strategies were not scheduled. |
| Static mapping across cycles | Accept for v1.0, defer cross-cycle rotation | Deterministic mapping is easier to reason about and test; rotation can be added later as a separate scheduler policy if needed. |
| Sort symbols before queue build | Preserve cache iteration order | `KlineCache` uses `unordered_map`; sorting gives deterministic queue and testable behavior. |
| Keep intervals grouped per strategy/symbol pair | Interleave intervals globally | User examples require all intervals of the pair before moving on. |
| Keep change in `WorkQueue` only | Modify `SignalEngine` loop or strategy registry | Smaller blast radius and clearer unit testing. |

---

## 11. Acceptance Criteria

- `WorkQueue::build()` produces zip `strategy + symbol` order.
- Strategy order follows `config.json` registration order.
- Symbol order is deterministic.
- Intervals remain grouped in each strategy's configured order.
- `strategies.size() > symbols.size()` skips unpaired strategies without crash and without enqueueing empty/null work items.
- `strategies.size() > symbols.size()` emits a warning at scan-cycle build time so strategy starvation is observable.
- Single-strategy mode pairs every symbol with that strategy.
- Existing `SignalEngine::processItem()` behavior remains unchanged.
- Unit tests cover zip ordering, static 1-1 mapping, symbol sorting, single-strategy mode, unpaired-strategy skip, empty registry, and empty symbols.
