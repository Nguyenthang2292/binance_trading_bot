# Zip Strategy Symbol Work Queue — Random Shuffle Edition

**Version:** 2.0
**Date:** 2026-05-17
**Status:** Ready for Implementation
**Supersedes:** `2026-05-16-zip-strategy-symbol-work-queue-v1.0.md`

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-05-16 | Initial zip scheduler: sorted symbol order, strategy-modulo assignment |
| 2.0 | 2026-05-17 | Replace sorted order with per-cycle random shuffle + injectable seed for testability |

---

## 1. Muc Tieu

Giu nguyen zip strategy/symbol scheduling da xac nhan o v1.0, nhung thay the sorted symbol order bang **per-cycle random shuffle** de loai bo tinh trang tail-symbol starvation — hien tuong cac symbol dung cuoi alphabet bi bo qua co he thong khi scan cycle bi cat som do budget exhaustion.

**Van de cu (v1.0):**

```text
symbols sorted = [AAVEUSDT, BNBUSDT, BTCUSDT, ETHUSDT, SOLUSDT, ...]
```

Voi Gemini budget thap, cac symbol dau alphabet (`AAVEUSDT`, `BNBUSDT`) luon duoc danh gia; cac symbol cuoi (`SOLUSDT`, `XRPUSDT`) bi bo qua co dinh — khong phai random, ma co he thong.

**Muc tieu v2.0:**

```text
cycle N:   [SOLUSDT, BTCUSDT, BNBUSDT, ETHUSDT, ...]   ← shuffled
cycle N+1: [ETHUSDT, SOLUSDT, XRPUSDT, BTCUSDT, ...]   ← re-shuffled
```

Moi symbol co xac suat duoc dat vao cac vi tri dau queue nhu nhau theo thoi gian, bat ke vi tri trong alphabet.

Luu y quan trong: v2.0 chi cai thien fairness theo **symbol position** trong zip queue. No khong bien zip scheduler thanh exhaustive strategy-symbol coverage. Moi symbol van chi duoc assign cho mot strategy slot trong mot cycle, va mapping strategy duoc quyet dinh boi vi tri sau shuffle + modulo.

**Non-goals:**

- Khong thay doi logic `evaluate()` cua bat ky strategy nao.
- Khong persistent shuffle state qua cac scan cycle (no deck/bag).
- Khong thay doi modulo assignment strategy→symbol.
- Khong thay doi interval ordering.
- Khong them config option de set seed tu `config.json`.
- Khong thay doi warning behavior khi `strategies.size() > symbols.size()`.

---

## 2. Hien Trang (v1.0)

`WorkQueue::build()` hien tai:

```text
strategies = registry.all()
symbols = std::sort(copy of scanner symbols)   ← deterministic, alphabetical

for symbolIndex in range(symbols.size()):
  strategy = strategies[symbolIndex % strategies.size()]
  symbol = symbols[symbolIndex]
  for interval in strategy.config().intervals:
    enqueue WorkItem(symbol, interval, strategy)
```

He qua: voi budget cut-off, tail symbols bi skip moi cycle.

---

## 3. Understanding Summary

- **What**: Thay `std::sort` bang `std::shuffle` trong `WorkQueue::build()`, voi injectable seed de unit tests co the assert deterministic behavior trong cung toolchain.
- **Why**: Tail-symbol starvation la systematic bias; shuffle bien no thanh probabilistic symbol-position fairness.
- **Who**: Operator chay trading bot voi nhieu symbol va Gemini budget gioi han.
- **Constraints**: Chi thay doi trong `WorkQueue`; interface change nho (optional seed param); warning behavior giu nguyen.
- **Non-goals**: No persistent deck, no config-driven seed, no abstraction layer.

---

## 4. Assumptions Va Constraints

### 4.1 Assumptions

| # | Assumption |
|---|---|
| A1 | `std::mt19937_64` seeded tu `std::random_device` la RNG production mac dinh; entropy chat luong cao phu thuoc standard library/OS. |
| A2 | Seed injection chi dung cho unit tests, khong expose ra `config.json`. |
| A3 | Symbol sort bi **thay the hoan toan** boi shuffle — khong sort truoc roi shuffle. |
| A4 | Moi scan cycle tao RNG moi tu seed (hoac `random_device`); khong chia se RNG state giua cac cycle. |
| A5 | Shuffle chi ap dung cho symbols; strategy order van tu `registry.all()` (thu tu `config.json`). |
| A6 | Warning khi `strategies.size() > symbols.size()` giu nguyen — random shuffle khong lam giam severity cua strategy starvation. |

### 4.2 Non-Functional Requirements

| Category | Requirement |
|---|---|
| Performance | `std::shuffle` la O(n) — khong tang complexity so voi `std::sort` O(n log n). |
| Scale | Ho tro hang tram symbols; RNG state la O(1). |
| Reliability | Moi cycle doc lap; loi o cycle N khong anh huong cycle N+1. |
| Testability | Injectable seed cho phep unit test assert deterministic queue order trong cung toolchain ma khong dung mock. |
| Observability | Khong them log moi; existing logs van ghi strategy/symbol/interval per work item. |
| Security | `random_device` dung cho production; khong dung `rand()` hay `srand(time(0))`. |

---

## 5. Design

### 5.1 Interface Change

```cpp
// work_queue.h

class WorkQueue {
public:
    static std::vector<WorkItem> build(
        const std::vector<std::string>& symbols,
        const strategy::StrategyRegistry& registry,
        std::optional<uint64_t> seed = std::nullopt  // nullopt → random_device
    );
};
```

Thay doi nho: them mot optional parameter vao cuoi va **giu nguyen return type `std::vector<WorkItem>`** de khop code hien tai. Tat ca call sites hien tai (khong truyen seed) van compile va hoat dong voi production-random behavior.

### 5.2 Implementation

```cpp
// work_queue.cpp

std::vector<WorkItem> WorkQueue::build(
    const std::vector<std::string>& symbols,
    const strategy::StrategyRegistry& registry,
    std::optional<uint64_t> seed)
{
    std::vector<WorkItem> out;
    const auto strategies = registry.all();

    if (strategies.empty() || symbols.empty()) {
        return out;
    }

    if (strategies.size() > symbols.size()) {
        const size_t unscheduled = strategies.size() - symbols.size();
        Logger::instance().log(
            LogLevel::Warning,
            "work queue strategy starvation this cycle unscheduled=" + std::to_string(unscheduled) +
                " symbols=" + std::to_string(symbols.size()) +
                " strategies=" + std::to_string(strategies.size()));
    }

    // Shuffle symbols per-cycle (replaces std::sort in v1.0)
    std::vector<std::string> shuffled = symbols;
    uint64_t rngSeed = seed.has_value()
        ? seed.value()
        : std::random_device{}();
    std::mt19937_64 rng(rngSeed);
    std::shuffle(shuffled.begin(), shuffled.end(), rng);

    // Zip strategy + symbol (unchanged from v1.0)
    for (size_t i = 0; i < shuffled.size(); ++i) {
        const auto* strategy = strategies[i % strategies.size()];
        if (!strategy) continue;

        const auto& symbol = shuffled[i];
        for (const auto& interval : strategy->config().intervals) {
            out.push_back(WorkItem{symbol, interval, strategy});
        }
    }

    return out;
}
```

**Diem khac biet chinh so voi v1.0:**

| V1.0 | V2.0 |
|---|---|
| `std::sort(shuffled.begin(), shuffled.end())` | `std::shuffle(shuffled.begin(), shuffled.end(), rng)` |
| Khong co seed parameter | `std::optional<uint64_t> seed = std::nullopt` |
| Deterministic order | Per-cycle random order |
| `std::vector<WorkItem>` return type | Giu nguyen `std::vector<WorkItem>` return type |

Phan con lai (modulo assignment, interval grouping, warning, null-strategy skip) **giu nguyen hoan toan**.

### 5.3 Scan Cycle Flow (Updated)

```text
SignalEngine::runScanCycle()
  symbols = scanner.symbols()          // unordered (unordered_map)
  queue = WorkQueue::build(symbols, registry)
                                       // internally: shuffle → zip → enqueue
  for item in queue:
    processItem(item)
```

`SignalEngine` khong thay doi.

### 5.4 Example

Input:

```text
strategies = [TrendBreakout (30m,1h,4h), Gartley (30m,1h,4h,1d)]
symbols = [BTCUSDT, ETHUSDT, SOLUSDT, BNBUSDT]  (raw, unordered)
```

Sau shuffle (gia su cycle N):

```text
shuffled = [SOLUSDT, BNBUSDT, BTCUSDT, ETHUSDT]
```

Queue output:

```text
TrendBreakout / SOLUSDT / 30m
TrendBreakout / SOLUSDT / 1h
TrendBreakout / SOLUSDT / 4h
Gartley       / BNBUSDT / 30m
Gartley       / BNBUSDT / 1h
Gartley       / BNBUSDT / 4h
Gartley       / BNBUSDT / 1d
TrendBreakout / BTCUSDT / 30m
TrendBreakout / BTCUSDT / 1h
TrendBreakout / BTCUSDT / 4h
Gartley       / ETHUSDT / 30m
Gartley       / ETHUSDT / 1h
Gartley       / ETHUSDT / 4h
Gartley       / ETHUSDT / 1d
```

Cycle N+1 se co shuffled order khac → cac strategy/symbol pair khac.

---

## 6. Edge Cases

| Case | Expected Behavior |
|---|---|
| No strategies | Queue empty. |
| No symbols | Queue empty. |
| Null strategy pointer | Skip (giu nguyen tu v1.0). |
| Strategy with no intervals | No work items for that pair (giu nguyen tu v1.0). |
| 1 strategy, N symbols | Moi symbol duoc pair voi strategy duy nhat; shuffle van ap dung de randomize order. |
| More symbols than strategies | Strategy assignment wraps with modulo, symbols duoc chon theo shuffled order. |
| More strategies than symbols | Warning emit; chi `symbols.size()` strategies dau duoc pair. |
| 1 symbol, N strategies | Chi `strategies[0]` runs; warning emit neu N > 1. |
| `strategies.size() == symbols.size()` | 1-1 mapping; no modulo wrap; shuffled order quyet dinh strategy nao danh gia symbol nao trong cycle do. |
| Same seed injected twice | Same queue order trong cung toolchain/build — deterministic replay cho unit tests. |
| `random_device` unavailable/degraded entropy | Behavior phu thuoc standard library; queue van build duoc, nhung production randomness co the kem hon tren environment do. |

---

## 7. Testing Strategy

### 7.1 Unit Tests — Update `tests/test_work_queue.cpp`

**Tests moi / updated:**

- `SeededShufflePreservesZipAssignments`
  - Inject fixed seed
  - Assert output symbol sequence la permutation cua input symbols theo **assigned symbol slots**
  - Assert strategy assignment dung theo shuffled position + modulo
  - Assert interval order dung theo strategy config

- `SameSeedProducesSameOrder`
  - Build queue hai lan voi cung seed
  - Assert queue order identical

- `FixedSeedCanProduceNonInputOrder`
  - Build voi fixed seed da verify trong repo toolchain
  - Assert assigned symbol slot order khac input order cho fixture co du symbol
  - Neu toolchain thay doi lam seed nay khong shuffle khac input order, doi seed fixture trong test; khong assert random production behavior

- `AllSymbolsAreAssignedExactlyOnce`
  - 3 strategies, 7 symbols, fixed seed
  - Collapse queue theo contiguous symbol/strategy pair
  - Assert moi input symbol duoc assign dung mot pair
  - Assert work item count cua moi symbol bang so intervals cua strategy duoc assign

- `DegeneratesToSingleStrategyAcrossAllSymbols`
  - 1 strategy, multiple symbols, fixed seed
  - Assert moi symbol duoc pair voi strategy do

- `SkipsUnpairedStrategiesWhenStrategiesExceedSymbols`
  - 5 strategies, 2 symbols, fixed seed
  - Assert chi 2 strategies dau tao work items
  - Assert warning duoc emit

- `ReturnsEmptyWhenNoStrategies`

- `ReturnsEmptyWhenNoSymbols`

- `NulloptSeedUsesRandomDevice` *(smoke test)*
  - Build hai lan khong truyen seed
  - Khong assert order — chi assert queue size correct va khong crash

**Tests cu can update (v1.0 → v2.0):**

- `SortsSymbolsForDeterministicZipOrder` → **xoa** hoac rename thanh `SeededShufflePreservesZipAssignments`
- `BuildsOneToOneMappingWhenCountsMatch` → update: inject seed; assert 1-1 mapping van dung nhung order la shuffled

### 7.2 Regression Tests

```powershell
ctest --test-dir build --output-on-failure -R "WorkQueue|SignalEngine|Strategy"
```

---

## 8. Implementation Handoff

| File | Change |
|---|---|
| `src/engine/work_queue.h` | Them `std::optional<uint64_t> seed = std::nullopt` vao `build()` declaration |
| `src/engine/work_queue.cpp` | Thay `std::sort` bang seeded `std::shuffle`; them RNG init block |
| `tests/test_work_queue.cpp` | Update sort-based tests; them shuffle/seed tests (xem Section 7.1) |

**Implementation notes:**

- Include `<random>` va `<algorithm>` neu chua co.
- Dung `std::mt19937_64` thay vi `std::mt19937` de tranh seed collision o 32-bit range.
- Khong mutate `symbols` parameter (da dung local copy `shuffled`).
- Khong thay doi `SignalEngine::processItem()`.

---

## 9. Decision Log

| Decision | Alternatives | Rationale |
|---|---|---|
| Per-cycle shuffle (no persistent state) | Persistent deck/shuffle bag across cycles | Don gian hon, khong can manage state giua cycles, de test, khong co edge case khi symbol set thay doi |
| Replace sort with shuffle (not sort-then-shuffle) | Sort first, then shuffle | Sort truoc shuffle vo nghia — shuffle pha vo sort order; bo sort cung giam complexity tu O(n log n) xuong O(n) |
| Injectable seed via optional parameter | ShufflePolicy abstraction, stateful WorkQueue | YAGNI: chi co mot use case; optional param la thay doi nho nhat co the; khong thay doi caller code |
| `std::mt19937_64` seeded tu `std::random_device` | `rand()`, `srand(time(0))`, fixed seed in production | `random_device` la source tot nhat san co trong standard library; `mt19937_64` co period du lon; tranh tat ca pitfalls cua `rand()` |
| Giu nguyen warning khi strategies > symbols | Bo warning vi shuffle it nghiem trong hon | Random shuffle khong lam giam strategy starvation severity; warning van can thiet cho observability |
| Seed chi cho test, khong expose ra config.json | Them `work_queue_seed` vao config.json | Operator khong can control seed; determinism trong production la undesirable (che giau bias) |

---

## 10. Acceptance Criteria

- `WorkQueue::build()` tao shuffled symbol order moi cycle khi khong truyen seed.
- Cung seed → cung queue order trong cung toolchain/build (deterministic replay cho tests).
- Tests khong assert `different seeds must differ`; do co xac suat collision va `std::shuffle` order phu thuoc implementation.
- Strategy order van theo `config.json` registration order.
- Interval order van theo strategy config.
- Moi input symbol duoc assign dung mot strategy slot moi cycle.
- Moi symbol tao so work items bang so intervals cua strategy duoc assign; strategy khong co intervals tao 0 work items.
- Random shuffle chi dam bao fairness theo symbol position theo thoi gian; khong dam bao exhaustive strategy-symbol coverage.
- `strategies.size() > symbols.size()` van emit warning.
- `SignalEngine::processItem()` khong thay doi.
- Unit tests pass voi fixed seed; khong co flaky tests phu thuoc vao random order.
