# Strategy Plugin Catalog

**Version:** 1.0
**Date:** 2026-05-14
**Status:** ✅ DONE - Implemented

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-05-14 | Initial design: DLL plugin loader, C ABI boundary, folder scan, CLI --list-strategies, runtime strategy log |

---

## 1. Muc Tieu

Thiet ke mot khung quan ly danh sach strategy dang plugin DLL de:

1. **Load strategy khong can rebuild bot** — copy `.dll` vao `plugins/`, restart la co.
2. **Hien thi danh sach strategy hien co** — qua CLI flag `--list-strategies` va runtime log.
3. **Tach biet hoan toan khoi concrete strategy** — catalog chi quan ly lifecycle; implementation strategy nam trong DLL rieng.

Scope cua tai lieu nay la **infrastructure catalog**, khong phai cac strategy cu the.

---

## 2. Understanding Lock

### 2.1 Summary

- **Plugin loading**: Bot scan thu muc `plugins/` luc startup, load tat ca `.dll` tim thay. Them strategy = copy DLL vao folder, khong can rebuild bot hay sua config.
- **ABI boundary**: C ABI thuan — plugin export `createStrategy(const char* config_json)` va `destroyStrategy()`. `IStrategy` la abstract class voi pure virtual functions (vtable-safe). Data transfer qua JSON string, khong qua C++ STL types.
- **User interaction**:
  - `config.json` — khai bao params cho tung strategy type (match theo `"type"` field)
  - `--list-strategies` CLI flag — in danh sach strategy da load roi thoat, khong start trading
  - Runtime log — dinh ky log strategy nao active, signal gan nhat
- **Tich hop**: Catalog la lop phia tren `StrategyRegistry` da thiet ke trong `2026-05-14-market-scanner-strategy-engine-v1.0.md`. PluginLoader load DLL → goi `createStrategy()` → dang ky vao `StrategyRegistry`.

### 2.2 Assumptions

1. Tat ca DLL trong `plugins/` deu duoc tin tuong — khong co sandboxing.
2. Khong ho tro hot-reload; can restart bot de them/bot strategy.
3. Moi DLL chua dung mot strategy type (1 DLL = 1 strategy).
4. Plugin build cung MSVC version voi bot (C ABI giam thieu risk nhung khong loai tru hoan toan).
5. `--list-strategies` print theo stdout, khong qua logger.
6. Config params cua strategy van den tu `config.json` `strategies[]` array, match theo `"type"` field.

### 2.3 Non-Goals

- Hot-reload plugin khi dang chay
- Concrete strategy implementations
- PnL / win-rate tracking per strategy
- Plugin sandboxing / security isolation
- Plugin dependency management

---

## 3. Lien Ket Voi Thiet Ke Hien Co

| Component | File | Vai tro |
|---|---|---|
| `IStrategy` interface | `src/strategy/istrategy.h` | Plugin DLL implement interface nay |
| `StrategyConfig` | `src/strategy/strategy_config.h` | Plugin nhan config duoi dang JSON string, parse thanh `StrategyConfig` |
| `StrategyRegistry` | `src/strategy/strategy_registry.h` | `PluginLoader` dang ky instance vao registry sau khi load |
| `config.json` | `config.json` | Them section `plugins` va extend `strategies[]` voi `"type"` field |

---

## 4. Module Layout

```
src/
  catalog/
    plugin_loader.h         # scan plugins/, load DLL, goi C ABI entry points
    plugin_loader.cpp
    plugin_handle.h         # RAII wrapper cho HMODULE (Windows) / dlopen (Linux)
    plugin_handle.cpp
    strategy_catalog.h      # orchestrator: PluginLoader + StrategyRegistry
    strategy_catalog.cpp
    catalog_reporter.h      # --list-strategies output + runtime log formatting
    catalog_reporter.cpp

plugins/                    # thu muc chua .dll files (khong commit vao git)
  .gitkeep

tests/
  test_plugin_loader.cpp
  test_strategy_catalog.cpp
  test_catalog_reporter.cpp

docs/sdk/
  writing-a-strategy-plugin.md  # huong dan viet plugin DLL moi
```

---

## 5. C ABI Contract

Moi plugin DLL PHAI export hai ham sau voi C linkage:

```cpp
// plugin exports — phai nam trong extern "C" block
extern "C" {

// Tao mot IStrategy instance moi.
// config_json: UTF-8 JSON string chua strategy config tu config.json.
//   Vi du: {"name":"rsi_v1","risk_pct":0.01,"atr_period":14}
// Tra ve: pointer den heap-allocated IStrategy; caller so huu.
// Tra ve nullptr neu config khong hop le.
__declspec(dllexport) IStrategy* createStrategy(const char* config_json);

// Huy IStrategy instance duoc tao boi createStrategy().
// Bat buoc: khong de caller goi delete truc tiep (heap khac nhau giua DLL va host).
__declspec(dllexport) void destroyStrategy(IStrategy* strategy);

// Tra ve ten strategy type nay (ASCII, null-terminated).
// Vi du: "rsi_reversal", "ema_cross"
// Dung de match voi "type" field trong config.json.
__declspec(dllexport) const char* strategyType();

// Tra ve version string cua plugin (ASCII, null-terminated).
// Vi du: "1.0.0"
__declspec(dllexport) const char* pluginVersion();

} // extern "C"
```

**Quy uoc an toan**:
- Plugin khong duoc tra ve `std::string`, `std::vector`, hay bat ky STL type nao qua boundary.
- Moi string tra ve la string literal hoac static buffer — khong can caller free.
- `IStrategy*` tao qua `createStrategy()` phai duoc giai phong bang `destroyStrategy()`, khong bang `delete`.
- Exceptions khong duoc vuot qua boundary — plugin phai catch het va tra ve `nullptr`.

---

## 6. Proposed Types

### 6.1 PluginHandle (RAII)

```cpp
namespace catalog {

// RAII wrapper cho loaded DLL.
// Giu HMODULE (Windows) hoac void* dlopen (Linux).
// Destructor goi FreeLibrary / dlclose.
class PluginHandle {
public:
    using CreateFn  = IStrategy*(*)(const char*);
    using DestroyFn = void(*)(IStrategy*);
    using TypeFn    = const char*(*)();
    using VersionFn = const char*(*)();

    static std::expected<PluginHandle, std::string> load(const std::filesystem::path& dll_path);

    PluginHandle(PluginHandle&&) noexcept;
    PluginHandle& operator=(PluginHandle&&) noexcept;
    ~PluginHandle();

    CreateFn  createFn  = nullptr;
    DestroyFn destroyFn = nullptr;
    TypeFn    typeFn    = nullptr;
    VersionFn versionFn = nullptr;

    std::filesystem::path path;
    std::string type;     // ket qua cua typeFn()
    std::string version;  // ket qua cua versionFn()

private:
    void* m_handle = nullptr; // HMODULE tren Windows
};

} // namespace catalog
```

### 6.2 PluginLoader

```cpp
namespace catalog {

struct PluginLoadResult {
    std::filesystem::path path;
    std::string type;
    std::string version;
    bool success;
    std::string error; // empty neu success
};

class PluginLoader {
public:
    struct Config {
        std::filesystem::path plugins_dir{"plugins"};
    };

    explicit PluginLoader(Config config);

    // Scan plugins_dir, load tat ca .dll, tra ve ket qua tung file.
    // Khong throw — loi load duoc ghi vao PluginLoadResult::error.
    std::vector<PluginLoadResult> loadAll();

    // Tao IStrategy instance tu plugin co type == strategy_type.
    // config_json: JSON string tu config.json.
    // Tra ve nullptr neu khong tim thay plugin hoac createFn tra nullptr.
    std::unique_ptr<IStrategy, void(*)(IStrategy*)> createStrategy(
        std::string_view strategy_type,
        const char* config_json
    );

    // Danh sach tat ca plugin da load thanh cong.
    const std::vector<PluginLoadResult>& loaded() const;

private:
    Config m_config;
    std::vector<PluginHandle> m_handles; // giu DLL con song
    std::vector<PluginLoadResult> m_results;
};

} // namespace catalog
```

### 6.3 StrategyCatalog

```cpp
namespace catalog {

class StrategyCatalog {
public:
    struct Config {
        std::filesystem::path plugins_dir{"plugins"};
    };

    StrategyCatalog(Config config, strategy::StrategyRegistry& registry);

    // 1. Load tat ca plugin tu plugins_dir
    // 2. Voi moi entry trong strategies_config: tim plugin theo "type",
    //    goi createStrategy(config_json), dang ky vao registry
    // 3. Tra ve summary
    struct LoadSummary {
        int plugins_found;
        int plugins_loaded;
        int strategies_registered;
        std::vector<std::string> errors;
    };
    LoadSummary initialize(const std::vector<nlohmann::json>& strategies_config);

    // Tra ve thong tin tat ca plugin + strategy da load
    struct StrategyInfo {
        std::string name;        // tu StrategyConfig::name
        std::string type;        // tu plugin::strategyType()
        std::string version;     // tu plugin::pluginVersion()
        std::string plugin_file; // ten file .dll
        std::vector<std::string> intervals;
    };
    std::vector<StrategyInfo> listStrategies() const;

private:
    Config m_config;
    strategy::StrategyRegistry& m_registry;
    PluginLoader m_loader;
    std::vector<StrategyInfo> m_info;
};

} // namespace catalog
```

### 6.4 CatalogReporter

```cpp
namespace catalog {

class CatalogReporter {
public:
    // In ra stdout: danh sach strategy, version, intervals, plugin file.
    // Dung cho --list-strategies CLI mode.
    static void printList(const std::vector<StrategyCatalog::StrategyInfo>& strategies);

    // In ra Logger: summary khi bot khoi dong.
    static void logStartupSummary(
        const StrategyCatalog::LoadSummary& summary,
        const std::vector<StrategyCatalog::StrategyInfo>& strategies
    );

    // In ra Logger: dinh ky trong runtime (moi scan_interval).
    // Hien thi strategy nao dang active, total symbols trong queue.
    static void logRuntimeStatus(
        const std::vector<StrategyCatalog::StrategyInfo>& strategies,
        int active_symbols,
        int open_positions
    );
};

} // namespace catalog
```

---

## 7. Config Schema

Extend `config.json` hien co:

```json
{
  "catalog": {
    "plugins_dir": "plugins"
  },
  "strategies": [
    {
      "name": "rsi_reversal_15m",
      "type": "rsi_reversal",
      "intervals": ["15m"],
      "scan_interval_seconds": 3600,
      "max_hold_duration_seconds": 86400,
      "risk_pct": 0.01,
      "sl_multiplier": 1.5,
      "tp_multiplier": 3.0,
      "takeProfitPercent": 20.0,
      "min_notional": 1.0,
      "atr_period": 14,
      "min_confidence": 0.5,
      "params": {
        "rsi_period": 14,
        "rsi_oversold": 30,
        "rsi_overbought": 70
      }
    }
  ]
}
```

`"type"` field dung de match voi `strategyType()` export cua plugin DLL.
`"params"` la extension tuy chon chua params rieng cua strategy, duoc forward nguyen vao `createStrategy(config_json)`.

---

## 8. CLI Integration

Trong `main.cpp`, kiem tra `argv` truoc khi khoi dong bot:

```cpp
int main(int argc, char* argv[]) {
    if (argc > 1 && std::string_view(argv[1]) == "--list-strategies") {
        // Load catalog, print list, exit 0
        auto config = loadConfig("config.json");
        strategy::StrategyRegistry registry;
        catalog::StrategyCatalog cat({config.plugins_dir}, registry);
        cat.initialize(config.strategies);
        catalog::CatalogReporter::printList(cat.listStrategies());
        return 0;
    }
    // ... khoi dong bot binh thuong
}
```

**Output mau `--list-strategies`:**

```
Loaded 2 strategies from 2 plugins in plugins/

  [1] rsi_reversal_15m
      Type    : rsi_reversal
      Version : 1.0.0
      Plugin  : strategy_rsi.dll
      Intervals: 15m

  [2] ema_cross_30m
      Type    : ema_cross
      Version : 1.2.0
      Plugin  : strategy_ema.dll
      Intervals: 30m
```

---

## 9. Runtime Log

Khi `SignalEngine` bat dau moi scan cycle, goi `CatalogReporter::logRuntimeStatus()`:

```
[INFO] Strategy status: 2 active | queue 3842 items | 5 open positions
       rsi_reversal_15m  → intervals: [15m]  scan: 3600s  hold: 86400s
       ema_cross_30m     → intervals: [30m]  scan: 7200s  hold: 43200s
```

---

## 10. Plugin SDK (Huong Dan Viet Plugin Moi)

Nguoi viet strategy can:

1. **Tao Visual Studio project** (DLL project), cung MSVC version voi bot.
2. **Include** `src/strategy/istrategy.h` va `src/strategy/strategy_config.h` tu bot repo.
3. **Implement** class ke thua `IStrategy`.
4. **Export C ABI** 4 functions: `createStrategy`, `destroyStrategy`, `strategyType`, `pluginVersion`.
5. **Build** thành `.dll`, copy vao `plugins/`.

**Template plugin toi thieu:**

```cpp
// my_strategy.cpp
#include "istrategy.h"
#include <nlohmann/json.hpp>

class MyStrategy : public strategy::IStrategy {
public:
    explicit MyStrategy(const strategy::StrategyConfig& cfg) : m_cfg(cfg) {}
    const strategy::StrategyConfig& config() const override { return m_cfg; }
    strategy::Signal evaluate(std::string_view symbol,
                              std::string_view interval,
                              const std::vector<Kline>& klines) const override {
        // ... logic cua ban
        return strategy::Signal{};
    }
private:
    strategy::StrategyConfig m_cfg;
};

extern "C" {
    __declspec(dllexport) IStrategy* createStrategy(const char* config_json) try {
        auto j = nlohmann::json::parse(config_json);
        strategy::StrategyConfig cfg;
        cfg.name = j.value("name", "my_strategy");
        // ... parse params
        return new MyStrategy(cfg);
    } catch (...) { return nullptr; }

    __declspec(dllexport) void destroyStrategy(IStrategy* s) { delete s; }
    __declspec(dllexport) const char* strategyType() { return "my_strategy"; }
    __declspec(dllexport) const char* pluginVersion() { return "1.0.0"; }
}
```

---

## 11. Error Handling

| Scenario | Xu ly |
|---|---|
| `plugins/` khong ton tai | Log warning, tiep tuc khoi dong voi 0 strategy |
| DLL khong export du 4 functions | `PluginLoadResult::error`, skip DLL do, tiep tuc |
| `createStrategy()` tra nullptr | Log error kem ten file + type, skip strategy do |
| JSON config malformed | Plugin phai catch va tra nullptr; bot log loi |
| DLL load loi (corrupt, sai arch) | `LoadLibrary` fail, ghi vao error, tiep tuc load cac DLL khac |
| Khong co strategy nao load duoc | Log warning; bot van chay nhung queue trong, khong co lenh |

---

## 12. Testing Strategy

| Test file | Scope |
|---|---|
| `test_plugin_loader.cpp` | Load DLL gia (test double) voi dung/sai exports; verify error capture; verify RAII cleanup |
| `test_strategy_catalog.cpp` | initialize() voi mock loader; verify registry nhan dung so strategy; verify type matching; verify error trong LoadSummary |
| `test_catalog_reporter.cpp` | printList output format; logStartupSummary format; khong crash khi list rong |

**Luu y**: Khong can DLL thuc trong unit test — dung function pointer mock thay the.

---

## 13. Phased Implementation Plan

### Phase P1 — PluginHandle + PluginLoader

- `src/catalog/plugin_handle.h/.cpp` — RAII Windows DLL loading
- `src/catalog/plugin_loader.h/.cpp` — scan + load + createStrategy factory
- Test: `test_plugin_loader.cpp` voi DLL gia

### Phase P2 — StrategyCatalog + Config Integration

- `src/catalog/strategy_catalog.h/.cpp` — tich hop PluginLoader va StrategyRegistry
- Extend `config.json` schema voi `catalog` va `strategies[].type`
- Test: `test_strategy_catalog.cpp`

### Phase P3 — CatalogReporter + CLI

- `src/catalog/catalog_reporter.h/.cpp`
- Wire `--list-strategies` vao `main.cpp`
- Wire `logStartupSummary` vao startup sequence
- Wire `logRuntimeStatus` vao `SignalEngine` scan cycle
- Test: `test_catalog_reporter.cpp`

### Phase P4 — Plugin SDK Doc + First Plugin

- Viet `docs/sdk/writing-a-strategy-plugin.md`
- Tao template plugin project
- Build strategy dau tien thanh DLL, verify end-to-end

---

## 14. Decision Log

| Quyet dinh | Lua chon da xem xet | Ly do chon | Trang thai |
|---|---|---|---|
| Plugin DLL dynamic loading | Self-registration macro, explicit factory map | Them strategy khong can rebuild bot; tach biet hoan toan | Approved |
| C ABI boundary | C++ objects qua boundary, "same settings" constraint | Stable tren MSVC; tranh heap mismatch crash | Approved |
| Folder scan tu dong | Config list explicit, hybrid whitelist | Don gian nhat; khong can sua config de them strategy | Approved |
| 1 DLL = 1 strategy type | N strategies trong 1 DLL | De trace loi; de version tung strategy doc lap | Approved |
| Khong hot-reload | Hot-reload voi watcher | Phuc tap, de race condition; restart la chap nhan duoc cho v1 | Approved |
| `--list-strategies` print stdout, khong qua logger | Dung logger | Stdout de pipe/script; logger la cho runtime, khong cho CLI inspect | Approved |
| Config match theo `"type"` string field | Match theo filename, match theo index | Ro rang, khong phu thuoc thu muc hay thu tu load | Approved |
