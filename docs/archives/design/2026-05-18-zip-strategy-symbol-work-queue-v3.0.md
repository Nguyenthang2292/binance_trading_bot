# Zip Strategy Symbol Work Queue — Dual Shuffle Edition

**Version:** 3.0
**Date:** 2026-05-18
**Status:** ✅ DONE - Implemented
**Supersedes:** `2026-05-17-zip-strategy-symbol-work-queue-v2.0.md`

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-05-16 | Initial zip scheduler: sorted symbol order, strategy-modulo assignment |
| 2.0 | 2026-05-17 | Replace sorted symbol order with per-cycle random shuffle + injectable seed |
| 3.0 | 2026-05-18 | Also shuffle strategy list per cycle (same RNG, sequential); make WorkQueue the single scheduling warning source |

---

## 1. Muc Tieu

v2.0 giai quyet tail-symbol starvation bang cach shuffle symbol list moi cycle. Tuy nhien, strategy list van giu nguyen thu tu tu `registry.all()` (thu tu `config.json`). Khi `strategies.size() > symbols.size()`, cac strategy o cuoi list **khong bao gio duoc pair** trong bat ky cycle nao — day la systematic strategy starvation, cung root cause voi symbol starvation da duoc giai quyet o v2.0.

**Van de cu (v2.0):**

```text
strategies (fixed) = [S0, S1, S2, S3, S4]   ← 5 strategies, thu tu co dinh
symbols (shuffled) = [D, B, A]               ← 3 symbols

→ S0→D, S1→B, S2→A  (moi cycle, chi khac symbol order)
→ S3, S4: khong bao gio duoc pair — systematic starvation
```

**Muc tieu v3.0:**

```text
cycle N:   strategies=[S3,S1,S0], symbols=[D,A,B] → S3→D, S1→A, S0→B
cycle N+1: strategies=[S0,S4,S2], symbols=[C,B,A] → S0→C, S4→B, S2→A
cycle N+2: strategies=[S4,S2,S3], symbols=[B,D,C] → S4→B, S2→D, S3→C
```

Theo thoi gian, moi strategy co xac suat xuat hien o cac vi tri dau list nhu nhau, bat ke thu tu dang ky trong `config.json`. Fairness nay la **probabilistic fairness**, khong phai bounded guarantee: voi `N` strategies, `M` symbols, xac suat mot strategy cu the van chua duoc schedule sau `k` cycles la `((N-M)/N)^k` khi `N > M`.

**Non-goals:**

- Khong thay doi logic `evaluate()` cua bat ky strategy nao.
- Khong dam bao moi strategy chay dung mot lan moi cycle (modulo-wrapping giu nguyen).
- Khong them strategy-symbol affinity.
- Khong persistent shuffle state qua cac scan cycle.
- Khong thay doi modulo assignment logic.
- Khong thay doi interval ordering.
- Khong them config option de set seed tu `config.json`.
- Khong thay doi interface `WorkQueue::build()` (seed param da co tu v2.0).
- Khong dam bao moi strategy duoc schedule trong mot so cycle toi da; neu can bounded rotation thi can persistent deck/rotation state rieng.

---

## 2. Hien Trang (v2.0)

`WorkQueue::build()` hien tai:

```text
strategies = registry.all()          ← thu tu config.json, KHONG shuffle
symbols    = copy of scanner symbols
rng        = mt19937_64(seed hoac random_device)
shuffle(symbols, rng)                ← chi shuffle symbols

for symbolIndex in range(symbols.size()):
  strategy = strategies[symbolIndex % strategies.size()]   ← strategy order co dinh
  symbol   = shuffled_symbols[symbolIndex]
  for interval in strategy.config().intervals:
    enqueue WorkItem(symbol, interval, strategy)
```

He qua: khi `strategies.size() > symbols.size()`, chi `symbols.size()` strategies dau (theo config order) duoc pair moi cycle.

---

## 3. Understanding Summary

- **What**: Them `std::shuffle(strategies...)` truoc `std::shuffle(symbols...)` trong `WorkQueue::build()`, dung cung mot RNG (sequential state), cap nhat warning message, va bo warning duplicate trong `SignalEngine::runScanCycle()`.
- **Why**: Tail-strategy starvation la systematic bias khi strategies > symbols; shuffle bien no thanh probabilistic fairness theo thoi gian, tuong tu cach v2.0 xu ly symbol starvation.
- **Who**: Operator chay trading bot voi nhieu strategies va Gemini budget gioi han.
- **Key decisions**: Modulo-wrapping giu nguyen; 1 RNG duy nhat (shuffle strategies truoc, symbols sau — idiomatic C++, dam bao randomness); warning message cap nhat de phan anh "per-cycle unscheduled strategies, probabilistically fair over time".
- **Constraints**: Queue construction chi thay doi trong `WorkQueue`; interface khong thay doi; `SignalEngine::processItem()` khong thay doi; `SignalEngine::runScanCycle()` chi xoa warning scheduling duplicate.
- **Non-goals**: No affinity, no persistent deck, no config-driven seed, no extra seed param.

---

## 4. Assumptions Va Constraints

### 4.1 Assumptions

| # | Assumption |
|---|---|
| A1 | `std::mt19937_64` sequential dung cho 2 shuffle lien tiep dam bao randomness du tot — day la idiom chuan C++. |
| A2 | Shuffle strategies truoc, symbols sau (trat tu nay anh huong deterministic replay; tests phai giu dung trat tu nay). |
| A3 | `registry.all()` tra ve vector copy (khong phai reference) — viec bo `const` chi anh huong local variable. |
| A4 | Seed injection giu nguyen `std::optional<uint64_t>` — khong them param moi. |
| A5 | Modulo-wrapping giu nguyen: khi symbols > strategies, mot so strategies xuat hien nhieu lan per cycle — chap nhan duoc. |
| A6 | Warning khi `strategies.size() > symbols.size()` giu nguyen tai `WorkQueue::build()` va la scheduling warning source duy nhat. |
| A7 | Fairness la theo xac suat qua nhieu scan cycles; thiet ke khong co bounded guarantee neu random shuffle lien tiep khong chon mot strategy trong nhieu cycle. |

### 4.2 Non-Functional Requirements

| Category | Requirement |
|---|---|
| Performance | 2x `std::shuffle` van O(n) — khong tang complexity so voi v2.0. |
| Scale | Ho tro hang tram strategies va symbols; RNG state la O(1). |
| Reliability | Moi cycle doc lap; loi o cycle N khong anh huong cycle N+1. |
| Testability | Injectable seed cho phep unit test assert deterministic queue order (ca strategy order lan symbol order) trong cung toolchain. |
| Observability | Cap nhat warning message; `WorkQueue::build()` la source duy nhat cho warning `strategies.size() > symbols.size()` de tranh duplicate/misleading logs. |
| Security | `random_device` dung cho production; khong dung `rand()` hay `srand(time(0))`. |

---

## 5. Design

### 5.1 Interface Change

```cpp
// work_queue.h — KHONG THAY DOI so voi v2.0

class WorkQueue {
public:
    static std::vector<WorkItem> build(
        const std::vector<std::string>& symbols,
        const strategy::StrategyRegistry& registry,
        std::optional<uint64_t> seed = std::nullopt
    );
};
```

Interface giu nguyen hoan toan. Tat ca call sites hien tai compile va hoat dong khong can sua.

### 5.2 Implementation

```cpp
// work_queue.cpp

std::vector<WorkItem> WorkQueue::build(
    const std::vector<std::string>& symbols,
    const strategy::StrategyRegistry& registry,
    std::optional<uint64_t> seed)
{
    std::vector<WorkItem> out;
    auto strategies = registry.all();    // ← bo const (v2.0: const auto) de shuffle

    if (strategies.empty() || symbols.empty()) {
        return out;
    }

    uint64_t rngSeed = seed.has_value()
        ? seed.value()
        : std::random_device{}();
    std::mt19937_64 rng(rngSeed);

    // Shuffle strategies per-cycle (THEM MOI o v3.0)
    std::shuffle(strategies.begin(), strategies.end(), rng);

    if (strategies.size() > symbols.size()) {
        const size_t unscheduled = strategies.size() - symbols.size();
        Logger::instance().log(
            LogLevel::Warning,
            "work queue per-cycle strategy rotation active"
            " unscheduled_this_cycle=" + std::to_string(unscheduled) +
            " symbols=" + std::to_string(symbols.size()) +
            " strategies=" + std::to_string(strategies.size()) +
            " (probabilistically fair over time via shuffle)");
    }

    // Shuffle symbols per-cycle (giu nguyen tu v2.0, cung RNG tiep tuc)
    std::vector<std::string> shuffled = symbols;
    std::shuffle(shuffled.begin(), shuffled.end(), rng);

    // Zip strategy + symbol (khong thay doi tu v1.0)
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

### 5.3 Diff So Voi v2.0

| v2.0 | v3.0 |
|---|---|
| `const auto strategies = registry.all()` | `auto strategies = registry.all()` |
| Khong co strategy shuffle | `std::shuffle(strategies.begin(), strategies.end(), rng)` truoc warning block |
| Warning: `"work queue strategy starvation this cycle..."` | Warning: `"work queue per-cycle strategy rotation active... (probabilistically fair over time via shuffle)"` |
| Symbol shuffle giu nguyen | Symbol shuffle giu nguyen (cung RNG, tiep tuc sau strategy shuffle) |

Phan con lai (modulo assignment, interval grouping, null-strategy skip, return type) **giu nguyen hoan toan**.

### 5.4 RNG Sequential Usage — Ly Do Dam Bao Randomness

```text
rng = mt19937_64(seed)
  → shuffle(strategies, rng): dung (strategies.size() - 1) buoc, state advance
  → shuffle(symbols, rng):    tiep tuc tu state moi — deterministic voi seed, khong can seed phu

Khong dung XOR seed derivation (de tranh correlation khi seed bit pattern tuong tu).
Sequential single-RNG la idiom chuan C++ cho nhieu shuffle lien tiep.
```

### 5.5 Example

Input:

```text
strategies = [S0(30m,1h), S1(30m,1h,4h), S2(1h), S3(30m), S4(1h,4h,1d)]
symbols = [BTCUSDT, ETHUSDT, SOLUSDT]  (3 symbols, 5 strategies)
```

Sau shuffle voi seed X (gia su cycle N):

```text
shuffled_strategies = [S3, S1, S4, S0, S2]
shuffled_symbols    = [SOLUSDT, BTCUSDT, ETHUSDT]

Warning: unscheduled_this_cycle=2 symbols=3 strategies=5 (probabilistically fair over time via shuffle)
```

Queue output:

```text
S3 / SOLUSDT / 30m
S1 / BTCUSDT / 30m
S1 / BTCUSDT / 1h
S1 / BTCUSDT / 4h
S4 / ETHUSDT / 1h        ← i=2, strategy index = 2%5 = 2 → S4
S4 / ETHUSDT / 4h
S4 / ETHUSDT / 1d
```

Cycle N+1 se co ca strategy order lan symbol order khac → cac pair khac nhau, S0 va S2 co co hoi duoc pair.

---

## 6. Edge Cases

| Case | Expected Behavior |
|---|---|
| No strategies | Queue empty. |
| No symbols | Queue empty. |
| Null strategy pointer | Skip (giu nguyen). |
| Strategy with no intervals | No work items for that pair (giu nguyen). |
| 1 strategy, N symbols | Strategy do duoc pair voi moi symbol; strategy shuffle tren 1 phan tu la no-op. |
| 1 symbol, N strategies | Chi strategy dau sau shuffle duoc pair; warning emit neu N > 1. |
| Strategies == symbols | 1-1 mapping; ca hai shuffled order quyet dinh pair. |
| Strategies < symbols | Tat ca strategies duoc pair (via modulo); symbol shuffle van dam bao fairness theo symbol. |
| Strategies > symbols | Warning emit; chi `symbols.size()` strategies dau (sau shuffle) duoc pair moi cycle — probabilistically fair over time, khong bounded. |
| Same seed injected twice | Same strategy order va same symbol order → same queue — deterministic replay. |
| `random_device` unavailable | Behavior phu thuoc standard library; queue van build, randomness co the kem hon. |

---

## 7. Testing Strategy

### 7.1 Unit Tests — Update `tests/test_work_queue.cpp`

**Tests moi / updated:**

- `SeededShuffleRandomizesStrategyOrder`
  - Inject fixed seed da verify trong repo toolchain
  - Trong test, tao expected order bang chinh algorithm: copy `registry.all()`, `std::mt19937_64(seed)`, `std::shuffle(strategies)`, copy symbols, `std::shuffle(symbols)`
  - Assert queue blocks match exact expected strategy-symbol zip order
  - Khong assert "must differ from input order" tren mot seed duy nhat vi shuffle co the hop le nhung trung order

- `SameSeedProducesSameStrategyAndSymbolOrder`
  - Build queue hai lan voi cung seed
  - Assert queue order identical (ca strategy assignment lan symbol order)

- `SeededDualShuffleUsesStrategyThenSymbolOrder`
  - Build expected order voi helper dung cung `std::mt19937_64`
  - Assert strategy shuffle duoc consume truoc symbol shuffle bang cach compare exact queue voi expected order
  - Test nay bao ve deterministic replay contract ma khong can goi "independent" theo nghia thong ke

- `AllStrategiesHavePositiveSelectionProbability`
  - 5 strategies, 2 symbols
  - Chay mot tap seed deterministic da chon san va kiem tra moi strategy xuat hien it nhat mot lan trong tap hop queue
  - Test nay la regression cho implementation shuffle strategy; khong duoc dien giai la production guarantee
  - Neu can test invariant chat hon, assert moi queue chi schedule prefix size `symbols.size()` cua expected shuffled strategies

- `ModuloWrappingPreservedAfterStrategyShuffle`
  - 2 strategies, 4 symbols, fixed seed
  - Assert moi strategy xuat hien dung 2 lan trong queue
  - Assert tong work items = sum(intervals cua moi strategy) * 2

- `AllSymbolsAssignedExactlyOnceAfterDualShuffle`
  - Giu nguyen tu v2.0: moi symbol van duoc assign dung mot slot
  - Update: inject seed; assert strategy assignment reflect shuffled strategy order

- `DegenerateSingleStrategy`
  - 1 strategy, multiple symbols — strategy shuffle la no-op; giu nguyen behavior

- `SkipsUnpairedStrategiesWhenStrategiesExceedSymbols`
  - 5 strategies, 2 symbols, fixed seed
  - Assert chi 2 strategies (theo shuffled order) tao work items
  - Assert warning duoc emit mot lan tu `WorkQueue` voi message moi (kiem tra substring "probabilistically fair over time")

- `SignalEngineDoesNotEmitDuplicateStrategySchedulingWarning`
  - Exercise `runScanCycle()` voi `strategies.size() > symbols.size()` neu test harness hien co cho phep
  - Assert khong con log `"strategy scheduling warning policy=zip_strategy_symbol"` tu `SignalEngine`
  - Verified bang implementation review/grep-level regression; khong con string duplicate trong `src/engine/signal_engine.cpp`

- `ReturnsEmptyWhenNoStrategies` — giu nguyen

- `ReturnsEmptyWhenNoSymbols` — giu nguyen

- `NulloptSeedUsesRandomDevice` *(smoke test)* — giu nguyen

**Tests cu can update (v2.0 → v3.0):**

- `SeededShufflePreservesZipAssignments` → update: strategy assignment bay gio phu thuoc shuffled strategy order, khong phai config.json order
- `BuildsOneToOneMappingWhenCountsMatch` → update: assert 1-1 mapping voi ca hai list da shuffle
- Bat ky test nao hardcode strategy thu tu theo config.json → update de inject seed va assert theo shuffled order

### 7.2 Regression Tests

```powershell
ctest --test-dir build --output-on-failure -R "WorkQueue|SignalEngine|Strategy"
```

---

## 8. Implementation Handoff

| File | Change |
|---|---|
| `src/engine/work_queue.h` | Khong thay doi |
| `src/engine/work_queue.cpp` | (1) Bo `const` khoi `auto strategies`; (2) Them `std::shuffle(strategies...)` truoc warning; (3) Cap nhat warning message |
| `src/engine/signal_engine.cpp` | Xoa warning duplicate trong `runScanCycle()` cho case `strategies.size() > symbols.size()`; khong thay doi `processItem()` |
| `tests/test_work_queue.cpp` | Them tests moi; update tests gia dinh strategy order co dinh |

**Implementation notes:**

- `<random>` va `<algorithm>` da co tu v2.0, khong can them include.
- Shuffle strategies **truoc** symbols — trat tu nay anh huong deterministic replay, tests phai giu dung.
- Khong mutate `symbols` parameter (da dung local copy `shuffled`).
- Khong mutate `registry` — `registry.all()` tra ve copy, shuffle tren copy do.
- Khong thay doi `SignalEngine::processItem()`.
- Xoa block log duplicate `"strategy scheduling warning policy=zip_strategy_symbol"` trong `SignalEngine::runScanCycle()` de `WorkQueue::build()` la source duy nhat cho scheduling warning.

---

## 9. Decision Log

| Decision | Alternatives | Rationale |
|---|---|---|
| Shuffle ca strategies lan symbols | Chi shuffle symbols (v2.0) | Strategy order co dinh de lai systematic starvation khi strategies > symbols — cung root cause voi symbol starvation |
| Modulo-wrapping giu nguyen | Doi sang exhaustive round-robin | YAGNI; modulo la logic on dinh tu v1.0; probabilistic fairness theo thoi gian du dap ung yeu cau hien tai |
| 1 RNG sequential (strategies truoc, symbols sau) | 2 RNG doc lap; XOR seed derivation | Sequential single-RNG la idiom chuan C++; dam bao randomness tot hon XOR derivation; don gian hon 2 RNG |
| Bo `const` tren local `strategies` variable | Tao copy rieng de shuffle | `registry.all()` da tra ve copy; bo `const` tren local variable la thay doi nho nhat co the |
| Mot scheduling warning source tai `WorkQueue::build()` | Giu warning o ca `WorkQueue` va `SignalEngine`; chi log o `SignalEngine` | Tranh duplicate/misleading logs; `WorkQueue` la noi co day du context ve scheduler va seed behavior |
| Warning noi ro probabilistic fairness | Noi "fair over time"; giu "strategy starvation" | Shuffle loai bo systematic bias nhung khong dam bao bounded rotation; wording phai phan anh xac suat |
| Khong them seed param moi | Them `seed_strategies` rieng | YAGNI; 1 seed du cho deterministic replay; interface giu nguyen don gian |

---

## 10. Acceptance Criteria

- `WorkQueue::build()` shuffle ca strategy list lan symbol list moi cycle khi khong truyen seed.
- Cung seed → cung strategy order va cung symbol order → cung queue (deterministic replay cho tests).
- Strategy shuffle dung cung RNG, thuc hien truoc symbol shuffle.
- Modulo-wrapping giu nguyen: strategy co the xuat hien nhieu lan neu symbols > strategies.
- Moi input symbol van duoc assign dung mot strategy slot moi cycle.
- Khi `strategies.size() > symbols.size()`: `WorkQueue::build()` emit warning voi message moi chua "probabilistically fair over time via shuffle".
- `SignalEngine::runScanCycle()` khong emit duplicate warning `"strategy scheduling warning policy=zip_strategy_symbol"`.
- Unit tests pass voi fixed seed; khong co flaky tests.
- `src/engine/work_queue.h` khong thay doi.
- `SignalEngine::processItem()` khong thay doi.
- All existing regression tests pass (sau khi update cac tests gia dinh strategy order co dinh).
