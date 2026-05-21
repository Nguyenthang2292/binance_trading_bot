# Risk Profile System — Design v1.0

**Date:** 2026-05-20  
**Status:** Approved for implementation

---

## Overview

Gom các cấu hình rời rạc liên quan đến risk control và order sizing thành một khái niệm duy nhất: **risk profile**. Người dùng chọn một trong ba mức (Conservative / Moderate / Aggressive) bằng một field duy nhất trong `config.json`. Không cần sửa nhiều chỗ, không cần hiểu sâu từng config.

---

## Goals

- Thay đổi toàn bộ bộ tham số risk/sizing bằng một dòng config
- `moderate` khớp chính xác với giá trị hiện tại — không có breaking change khi migrate
- Không thay đổi runtime behavior khi `active_profile` vắng mặt: chạy legacy mode và giữ nguyên các field hiện có trong `engine`, `order_cap`, `exposure_control`, `risk_analytics`, `strategies`

## Non-goals

- Không auto-switch profile theo market condition hoặc drawdown trigger
- Không per-strategy profile values (mọi strategy dùng chung một bộ giá trị từ profile)
- Không hot-reload profile trong runtime

---

## Schema — `config.json`

Thêm hai key vào root:

```json
{
  "active_profile": "moderate",

  "risk_profiles": {
    "conservative": {
      "risk_pct": 0.005,
      "sl_multiplier": 2.0,
      "max_position_notional_x_available_balance": 0.25,
      "max_total_notional_pct": 4.0,
      "soft_max_drawdown": 0.10,
      "hard_max_drawdown": 0.20,
      "soft_min_upi": 1.0,
      "hard_min_upi": 0.0,
      "soft_limit_net_beta": 0.3,
      "hard_limit_net_beta": 0.5,
      "max_gross_beta": 1.5
    },
    "moderate": {
      "risk_pct": 0.01,
      "sl_multiplier": 1.5,
      "max_position_notional_x_available_balance": 0.5,
      "max_total_notional_pct": 8.0,
      "soft_max_drawdown": 0.20,
      "hard_max_drawdown": 0.35,
      "soft_min_upi": 0.5,
      "hard_min_upi": -1.0,
      "soft_limit_net_beta": 0.5,
      "hard_limit_net_beta": 1.0,
      "max_gross_beta": 3.0
    },
    "aggressive": {
      "risk_pct": 0.05,
      "sl_multiplier": 1.2,
      "max_position_notional_x_available_balance": 0.75,
      "max_total_notional_pct": 15.0,
      "soft_max_drawdown": 0.30,
      "hard_max_drawdown": 0.65,
      "soft_min_upi": 0.2,
      "hard_min_upi": -2.0,
      "soft_limit_net_beta": 0.8,
      "hard_limit_net_beta": 1.5,
      "max_gross_beta": 5.0
    }
  }
}
```

### Giá trị so sánh

| Config | Conservative | Moderate | Aggressive |
|--------|-------------|----------|------------|
| `risk_pct` | 0.5% | 1% | 5% |
| `sl_multiplier` | 2.0× ATR | 1.5× ATR | 1.2× ATR |
| `max_position_notional` | 25% balance | 50% balance | 75% balance |
| `max_total_notional_pct` | 4× | 8× | 15× |
| `soft_max_drawdown` | 10% | 20% | 30% |
| `hard_max_drawdown` | 20% | 35% | **65%** |
| `soft_min_upi` | 1.0 | 0.5 | 0.2 |
| `hard_min_upi` | 0.0 | −1.0 | −2.0 |
| `soft_limit_net_beta` | 0.3 | 0.5 | 0.8 |
| `hard_limit_net_beta` | 0.5 | 1.0 | 1.5 |
| `max_gross_beta` | 1.5 | 3.0 | 5.0 |

---

## C++ Struct

Thêm file mới: **`src/engine/risk_profile.h`**

```cpp
#pragma once

#include <nlohmann/json_fwd.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine {

struct RiskProfile {
    std::string name{"moderate"};

    // sizing (applies to all strategies)
    double riskPct{0.01};
    double slMultiplier{1.5};

    // engine guardrails
    double maxPositionNotionalXAvailableBalance{0.5};

    // order cap
    double maxTotalNotionalPct{8.0};

    // risk analytics
    double softMaxDrawdown{0.20};
    double hardMaxDrawdown{0.35};
    double softMinUpi{0.5};
    double hardMinUpi{-1.0};

    // exposure control
    double softLimitNetBeta{0.5};
    double hardLimitNetBeta{1.0};
    double maxGrossBeta{3.0};

    void validate() const;
};

struct RiskProfileLoadResult {
    bool enabled{false};
    RiskProfile profile{};
    std::vector<std::string> warnings;

    // Load active profile from root config JSON.
    // If active_profile is absent, profile mode is disabled and legacy config stays authoritative.
    // If active_profile is present but invalid/unknown, fall back to the hardcoded moderate profile.
    static RiskProfileLoadResult loadActive(const nlohmann::json& root);
};

} // namespace engine
```

### Implementation — `src/engine/risk_profile.cpp`

```cpp
#include "engine/risk_profile.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace engine {

void RiskProfile::validate() const {
    if (riskPct <= 0.0 || riskPct >= 1.0) {
        throw std::invalid_argument("risk profile risk_pct must be > 0 and < 1");
    }
    if (slMultiplier <= 0.0) {
        throw std::invalid_argument("risk profile sl_multiplier must be > 0");
    }
    if (maxPositionNotionalXAvailableBalance <= 0.0) {
        throw std::invalid_argument("risk profile max_position_notional_x_available_balance must be > 0");
    }
    if (maxTotalNotionalPct <= 0.0) {
        throw std::invalid_argument("risk profile max_total_notional_pct must be > 0");
    }
    if (softMaxDrawdown <= 0.0 || hardMaxDrawdown <= 0.0 || softMaxDrawdown > hardMaxDrawdown ||
        hardMaxDrawdown >= 1.0) {
        throw std::invalid_argument("risk profile drawdown thresholds are invalid");
    }
    if (hardMinUpi > softMinUpi) {
        throw std::invalid_argument("risk profile hard_min_upi must be <= soft_min_upi");
    }
    if (softLimitNetBeta < 0.0 || hardLimitNetBeta < 0.0 || softLimitNetBeta > hardLimitNetBeta) {
        throw std::invalid_argument("risk profile net beta thresholds are invalid");
    }
    if (maxGrossBeta <= 0.0) {
        throw std::invalid_argument("risk profile max_gross_beta must be > 0");
    }
}

RiskProfileLoadResult RiskProfileLoadResult::loadActive(const nlohmann::json& root) {
    RiskProfileLoadResult result;
    RiskProfile moderate; // holds the "moderate" defaults as fallback
    moderate.validate();

    if (!root.contains("active_profile")) {
        result.enabled = false; // legacy mode: do not override existing config fields
        return result;
    }

    if (!root.at("active_profile").is_string()) {
        result.enabled = true;
        result.profile = moderate;
        result.warnings.push_back("active_profile must be a string, using moderate");
        return result;
    }

    const std::string activeName = root.at("active_profile").get<std::string>();
    const auto& profiles = root.value("risk_profiles", nlohmann::json::object());

    if (!profiles.is_object() || !profiles.contains(activeName) || !profiles.at(activeName).is_object()) {
        result.enabled = true;
        result.profile = moderate;
        result.warnings.push_back("unknown or missing active risk profile " + activeName + ", using moderate");
        return result;
    }

    try {
        const auto& j = profiles.at(activeName);
        RiskProfile profile;
        profile.name                                  = activeName;
        profile.riskPct                               = j.value("risk_pct",                               moderate.riskPct);
        profile.slMultiplier                          = j.value("sl_multiplier",                          moderate.slMultiplier);
        profile.maxPositionNotionalXAvailableBalance  = j.value("max_position_notional_x_available_balance", moderate.maxPositionNotionalXAvailableBalance);
        profile.maxTotalNotionalPct                   = j.value("max_total_notional_pct",                 moderate.maxTotalNotionalPct);
        profile.softMaxDrawdown                       = j.value("soft_max_drawdown",                      moderate.softMaxDrawdown);
        profile.hardMaxDrawdown                       = j.value("hard_max_drawdown",                      moderate.hardMaxDrawdown);
        profile.softMinUpi                            = j.value("soft_min_upi",                           moderate.softMinUpi);
        profile.hardMinUpi                            = j.value("hard_min_upi",                           moderate.hardMinUpi);
        profile.softLimitNetBeta                      = j.value("soft_limit_net_beta",                    moderate.softLimitNetBeta);
        profile.hardLimitNetBeta                      = j.value("hard_limit_net_beta",                    moderate.hardLimitNetBeta);
        profile.maxGrossBeta                          = j.value("max_gross_beta",                         moderate.maxGrossBeta);
        profile.validate();
        result.profile = profile;
    } catch (const std::exception& e) {
        result.profile = moderate;
        result.warnings.push_back("invalid risk profile " + activeName + ": " + e.what() + "; using moderate");
    }
    result.enabled = true;
    return result;
}

} // namespace engine
```

---

## Loading Logic — `src/main.cpp`

Profile được load **một lần sau khi đọc config**, trước khi khởi tạo bất kỳ config struct nào. Profile mode chỉ bật khi root config có `active_profile`; nếu key này vắng mặt, toàn bộ config legacy giữ nguyên.

### Bước 1 — Load profile (thêm ngay sau khi parse `config`)

```cpp
// Load risk profile — must happen before any strategy/engine/risk config parsing.
const auto riskProfile = engine::RiskProfileLoadResult::loadActive(config);
for (const auto& warning : riskProfile.warnings) {
    Logger::instance().log(LogLevel::Warning, warning);
}
if (riskProfile.enabled) {
    const auto& profile = riskProfile.profile;
    Logger::instance().log(
        LogLevel::Info,
        "active risk profile: " + profile.name
        + " risk_pct=" + std::to_string(profile.riskPct)
        + " hard_drawdown=" + std::to_string(profile.hardMaxDrawdown));
} else {
    Logger::instance().log(LogLevel::Info, "risk profile disabled: using legacy config values");
}
```

### Bước 2 — Override strategy JSON trước khi truyền vào plugins

Vì mỗi plugin tự parse `risk_pct` / `sl_multiplier` từ JSON object của mình, cần inject profile vào từng strategy JSON **trước khi** `strategyCatalog.initialize()`:

```cpp
// In toStrategyConfigs() or after calling it — inject profile values
auto strategyConfigs = toStrategyConfigs(config);
if (riskProfile.enabled) {
    for (auto& stratJson : strategyConfigs) {
        stratJson["risk_pct"] = riskProfile.profile.riskPct;
        stratJson["sl_multiplier"] = riskProfile.profile.slMultiplier;
    }
}
```

### Bước 3 — Override `SignalEngine::Config`

```cpp
engine::SignalEngine::Config engineConfig;
engineConfig.minNotional = engineJson.value("min_notional", 1.0);
engineConfig.maxPositionNotionalXAvailableBalance =
    engineJson.value("max_position_notional_x_available_balance", 0.5);
if (riskProfile.enabled) {
    engineConfig.maxPositionNotionalXAvailableBalance =
        riskProfile.profile.maxPositionNotionalXAvailableBalance;
}
// ... rest of engine config
```

### Bước 4 — Override `OrderCapConfig`

```cpp
orderCapConfig.maxTotalNotionalPct =
    orderCapJson.value("max_total_notional_pct", orderCapConfig.maxTotalNotionalPct);
if (riskProfile.enabled) {
    orderCapConfig.maxTotalNotionalPct = riskProfile.profile.maxTotalNotionalPct;
}
```

### Bước 5 — Override `ExposureConfig`

```cpp
exposureConfig.softLimitNetBeta = exposureJson.value("soft_limit_net_beta", exposureConfig.softLimitNetBeta);
exposureConfig.hardLimitNetBeta = exposureJson.value("hard_limit_net_beta", exposureConfig.hardLimitNetBeta);
exposureConfig.maxGrossBeta = exposureJson.value("max_gross_beta", exposureConfig.maxGrossBeta);
if (riskProfile.enabled) {
    exposureConfig.softLimitNetBeta = riskProfile.profile.softLimitNetBeta;
    exposureConfig.hardLimitNetBeta = riskProfile.profile.hardLimitNetBeta;
    exposureConfig.maxGrossBeta = riskProfile.profile.maxGrossBeta;
}
```

### Bước 6 — Override `RiskConfig` (sau khi `fromJson`)

```cpp
riskConfig = engine::RiskConfig::fromJson(riskJson);
if (riskProfile.enabled) {
    riskConfig.softMaxDrawdown = riskProfile.profile.softMaxDrawdown;
    riskConfig.hardMaxDrawdown = riskProfile.profile.hardMaxDrawdown;
    riskConfig.softMinUpi      = riskProfile.profile.softMinUpi;
    riskConfig.hardMinUpi      = riskProfile.profile.hardMinUpi;
    riskConfig.validate(); // extract existing RiskConfig::fromJson validation into a reusable method
}
```

`RiskConfig::fromJson()` hiện validate threshold ngay trong hàm parse. Khi profile override sau `fromJson`, cần refactor phần validate đó thành `RiskConfig::validate()` và gọi cả trong `fromJson()` lẫn sau khi apply profile.

---

## Fallback & Validation

| Tình huống | Hành vi |
|-----------|---------|
| `active_profile` vắng mặt | Legacy mode: không apply profile, giữ nguyên config hiện có |
| `active_profile` có giá trị không hợp lệ (ví dụ `"ultra"`) | Log warning, dùng `moderate` |
| `risk_profiles` block vắng mặt hoàn toàn | Dùng `moderate` (hardcoded defaults) |
| Một field trong profile bị thiếu | Dùng giá trị default của `moderate` cho field đó |
| Một field trong profile sai type/range hoặc threshold bị đảo | Log warning, dùng `moderate` |

Không throw exception khi profile lỗi — system tiếp tục chạy với `moderate`.

Validation bắt buộc cho profile:

- `0 < risk_pct < 1`
- `sl_multiplier > 0`
- `max_position_notional_x_available_balance > 0`
- `max_total_notional_pct > 0` (đây là multiplier theo account balance, ví dụ `8.0` = 8×, không phải 8%)
- `0 < soft_max_drawdown <= hard_max_drawdown < 1`
- `hard_min_upi <= soft_min_upi`
- `0 <= soft_limit_net_beta <= hard_limit_net_beta`
- `max_gross_beta > 0`

`aggressive` cố ý cho phép risk capital tăng hơn 6× so với `moderate` vì `risk_pct / sl_multiplier` tăng từ `0.01 / 1.5` lên `0.05 / 1.2`. Đây là quyết định risk appetite đã được chấp nhận, không phải lỗi validation.

---

## Config fields KHÔNG bị ảnh hưởng bởi profile

Các field này giữ nguyên là global config, không bị override:

| Config | Lý do |
|--------|-------|
| `engine.min_notional` | Sàn kỹ thuật, phụ thuộc exchange |
| `exposure_control.target_net_beta` | Tham số chiến lược, không phải risk |
| `exposure_control.default_beta` | Fallback kỹ thuật |
| `exposure_control.min_notional_after_scale` | Sàn kỹ thuật |
| `exposure_control.beta_window_days` | Lookback window, không phải risk level |
| `*.failure_mode` | Profile không override; giữ nguyên config hiện có. Khuyến nghị production dùng `closed` |
| `*.enabled` | Feature flags |
| `risk_analytics.db_path` | Infrastructure |
| `risk_analytics.sample_interval_minutes` | Infrastructure |
| `risk_analytics.control_lookback_days` | Window analytics, không phải risk level |
| `loss_manager.*` | DCA logic riêng biệt |
| `gemini_filter.*` | AI filter, không phải risk sizing |

---

## Decision Log

| # | Quyết định | Thay thế đã xem xét | Lý do chọn |
|---|-----------|---------------------|------------|
| 1 | 3 mức: Conservative / Moderate / Aggressive | Market regime, Account stage | Đơn giản nhất, phù hợp usecase manual switching |
| 2 | Manual switching qua `active_profile` | Auto-trigger theo drawdown | Tránh phức tạp hóa, user có full control |
| 3 | Inline profile block trong `config.json` | File profile riêng biệt | Một file duy nhất, dễ diff và audit |
| 4 | Override cứng toàn bộ strategy | Multiplier trên giá trị gốc | Predictable, không phụ thuộc giá trị base của từng strategy |
| 5 | `moderate` = giá trị hiện tại | Đặt lại baseline | Zero breaking change khi migrate |
| 6 | Inject vào strategy JSON trước khi truyền vào plugin | Override sau khi parse StrategyConfig | Plugin tự parse JSON — inject trước là điểm can thiệp duy nhất không cần sửa plugin |
| 7 | Override `RiskConfig` sau `fromJson`, rồi validate lại | Parse profile trong `fromJson` | Tách biệt concern nhưng không bypass invariant validation |
| 8 | Thiếu `active_profile` = legacy mode | Thiếu `active_profile` = hardcoded `moderate` | Không làm mất custom legacy config của user khi chưa migrate |

---

## Implementation Checklist

- Thêm `src/engine/risk_profile.h` và `src/engine/risk_profile.cpp`
- Update `CMakeLists.txt`: thêm `risk_profile.cpp` vào `LIB_SOURCES`, `risk_profile.h` vào `LIB_HEADERS`
- Refactor `RiskConfig::fromJson()` để dùng chung `RiskConfig::validate()`
- Update `src/main.cpp` theo thứ tự: load profile result → build mutable strategy configs → apply overrides có điều kiện → validate lại `RiskConfig`
- Giữ `order_cap.max_total_notional_pct` là multiplier theo balance; update comment/test name nếu cần để tránh hiểu nhầm percent
- Add unit tests:
  - thiếu `active_profile` trả `enabled=false` và không override strategy/engine/risk config
  - unknown `active_profile` fallback `moderate` kèm warning
  - missing field trong selected profile dùng default `moderate`
  - invalid profile range/cross-field fallback `moderate`
  - profile override strategy JSON trước `strategyCatalog.initialize()`
  - profile override `RiskConfig` xong vẫn gọi validation và reject/fallback threshold invalid
