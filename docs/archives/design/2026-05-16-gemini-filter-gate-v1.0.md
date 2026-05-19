# ✅ Gemini Filter Gate — Design Document

**Version:** 1.1  
**Date:** 2026-05-16  
**Status:** ✅ DONE - Implemented

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.1 | 2026-05-16 | Review fixes: migrate to `google-genai`, run Python via `-m`, remove global SDK state, pin models, add shadow/enforce rollout modes, add scan-cycle budget, use unique evaluation temp directories, use structured output validation, clarify current `openPosition(signalInterval)` integration |
| 1.0 | 2026-05-16 | Initial design: `IGeminiFilterPort`, `GeminiFilterController`, Python subprocess wrapper, Gemini sentiment + vision scoring, SignalEngine integration after ExposureController |

---

## 1. Mục Tiêu

Thiết kế `GeminiFilterController` — bộ lọc AI cuối cùng trong `SignalEngine::openPosition()`, đánh giá chất lượng signal trước khi mở lệnh bằng hai nguồn độc lập:

1. **Sentiment** — Gemini với Google Search grounding đánh giá tin tức, tâm lý thị trường của symbol và macro crypto context.
2. **Vision** — OHLCV được render thành chart candlestick multi-timeframe, Gemini Vision đánh giá trend, pattern, momentum và mức độ xác nhận signal.

Kết quả được tổng hợp thành `confidence`. Trong `mode="enforce"`, lệnh chỉ được mở khi Gemini quyết định `Allow`. Trong `mode="shadow"`, Gemini vẫn chạy và log `would_allow/would_block`, nhưng không chặn lệnh.

**Vấn đề hiện tại:**

- `SignalEngine` chỉ filter theo `minConfidence` từ strategy và `ExposureController` beta exposure.
- Strategy hiện chủ yếu dùng technical signals, không có external market context.
- False entries có thể xảy ra khi technical signal valid nhưng macro sentiment hoặc chart context không xác nhận.

**Phạm vi v1.1:**

1. `IGeminiFilterPort` + `GeminiFilterController` + `NoOpGeminiFilterPort` trong C++.
2. Python package `tools/gemini_filter/` dùng SDK hiện hành `google-genai`.
3. Integration vào `SignalEngine::openPosition()` sau ExposureController và trước order placement.
4. Config `gemini_filter` trong `config.json`; Gemini API keys trong `.env`.
5. Rollout an toàn bằng `disabled`/`shadow`/`enforce`, budget theo scan cycle và fail-closed chỉ khi enforce.

---

## 2. Review Fix Lock

Các quyết định dưới đây là bắt buộc để document ở trạng thái implement-ready:

| Finding đã fix | Quyết định v1.1 |
|---|---|
| SDK Python cũ `google-generativeai` | Dùng `google-genai` (`from google import genai`, `from google.genai import types`) |
| Chạy script trực tiếp làm hỏng relative import | C++ gọi `python -m tools.gemini_filter.gemini_filter <input_file>` |
| `genai.configure()` global state race giữa 2 thread | Không dùng global configure; mỗi call tạo `genai.Client(api_key=key)` riêng |
| Dynamic model discovery chọn sai capability | Không auto-discover trong hot path; model được pin trong config và lỗi rõ ràng nếu unsupported |
| Multiple subprocess đều bắt đầu bằng key 1 | Mỗi subprocess chọn random start index, sau đó rotate qua toàn bộ keys trong process |
| Blocking Gemini làm scan cycle trễ không giới hạn | Thêm `max_evaluations_per_scan_cycle` và timeout cứng; budget exhausted xử lý theo mode |
| Temp filename collision và cleanup timeout không chắc chắn | Mỗi evaluation dùng UUID directory riêng; C++ cleanup recursively kể cả timeout; Python cleanup best-effort |
| Regex JSON fallback biến lỗi thành neutral | Dùng structured output schema + validation; parse/validation fail là component error, không fallback `0.5` |
| Default enforce/fail-closed quá rủi ro | Default `enabled=false`; khi bật, default `mode="shadow"`; enforce chỉ bật sau smoke/shadow validation |
| Tài liệu nói thêm `primaryTf` dù code đã có interval | Reuse tham số hiện có `signalInterval` làm primary timeframe; không thêm tham số dư |

---

## 3. Runtime Modes

| Mode | Subprocess Gemini | Có chặn lệnh không? | Khi lỗi/timeout/budget exhausted |
|---|---:|---:|---|
| `disabled` | Không | Không | `NoOpGeminiFilterPort` luôn Allow |
| `shadow` | Có | Không | Log `would_block`, order vẫn tiếp tục |
| `enforce` | Có | Có | Fail-closed: Block |

**Default production-safe:**

- `enabled=false` trong config mẫu để không thay đổi behavior hiện tại khi deploy.
- Khi bắt đầu rollout, set `enabled=true`, `mode="shadow"` trong ít nhất vài scan cycle.
- Chỉ chuyển `mode="enforce"` sau khi xác nhận latency, quota, stdout JSON, cleanup và log ổn định.

---

## 4. Assumptions

| # | Assumption |
|---|---|
| A1 | Python >= 3.11 có sẵn; dependencies trong `tools/gemini_filter/requirements.txt` đã install |
| A2 | `.env` chứa ít nhất 1 key dạng `GEMINI_API_KEY_1`, `GEMINI_API_KEY_2`, ... hoặc fallback `GEMINI_API_KEY`; legacy `GEMINI_API_KEYS` và `GEMINI_TEXT_API_KEY` được hỗ trợ để tương thích config hiện có |
| A3 | Key rotation là per-subprocess: random start index + thử lần lượt toàn bộ keys; không claim global round-robin across subprocess |
| A4 | `sentiment_model` và `vision_model` được pin trong config; không dùng `list_models()` để tự chọn model trong hot path |
| A5 | Google Search + structured output dùng model hỗ trợ combination đó; default model chọn Gemini 3.1 Pro Preview vì API smoke test xác nhận hỗ trợ text, vision, Search và structured output |
| A6 | `runtime_dir` được tạo tự động; mỗi evaluation tạo subdirectory `eval-{uuid}` riêng |
| A7 | C++ xóa evaluation directory trong mọi path có thể kiểm soát, kể cả timeout; Python cũng cleanup chart best-effort |
| A8 | OHLCV extra TF lấy từ `KlineCache` snapshot hiện tại; extra TF thiếu thì skip, primary TF thiếu là component error |
| A9 | Timeout mặc định 45s; subprocess timeout trong enforce => Block, trong shadow => log only |
| A10 | `GeminiFilterController::evaluate()` là synchronous bounded call; SignalEngine giới hạn số lần gọi bằng `max_evaluations_per_scan_cycle` |

---

## 5. Non-Goals

- Không backtest sentiment/vision scoring trong scope này.
- Không dùng Gemini để quyết định sizing.
- Không lưu full prompt/response chứa dữ liệu nhạy cảm; chỉ log summary, scores và error code.
- Không chạy persistent Python sidecar/server trong v1.1.
- Không thêm per-strategy override trong v1.1; filter global cho mọi strategy.
- Không đảm bảo global fair key scheduling giữa nhiều bot process; nếu cần sẽ thiết kế sidecar hoặc shared state lock ở version sau.

---

## 6. Non-Functional Requirements

| Attribute | Requirement |
|---|---|
| Latency per evaluation | `timeout_seconds` default 10s, hard cap bằng C++ process timeout |
| Scan-cycle budget | `max_evaluations_per_scan_cycle` default 3, nên giữ `max * timeout <= 30s` với scan interval hiện tại 900s |
| Failure behavior | `enforce` fail-closed; `shadow` không block; `disabled` no-op |
| Thread safety | Không shared mutable SDK global state; each Gemini call has its own `genai.Client` |
| Disk safety | Unique eval dir per request; recursive cleanup; startup stale cleanup older than TTL |
| API cost | Enforce budget; log calls per cycle; sentiment may be 1 call or 2 calls if fallback `search_then_score=true` |
| Observability | Log mode, decision, confidence, component scores, latency_ms, error_code, eval_id; never log API keys |
| Testability | C++ mock port; Python unit tests mock Gemini client and chart generation |

---

## 7. Current Project State

| Component | File | Integration note |
|---|---|---|
| SignalEngine | `src/engine/signal_engine.h/.cpp` | `openPosition()` already has `signalInterval`; reuse it as primary TF |
| ExposureController | `src/engine/exposure_controller.h/.cpp` | Gemini gate runs only after exposure Allow/ScaleDown passes |
| KlineCache | `src/scanner/kline_cache.h/.cpp` | `snapshot(symbol, interval)` supplies OHLCV data |
| WorkItem | `src/engine/work_queue.h` | `item.interval` is passed into `openPosition()` today |
| Config parsing | `src/main.cpp` | Add `gemini_filter` parsing and wiring |
| Scanner intervals | `config.json` | Current intervals include `1d`, `4h`, `1h`, `30m`; default extra TFs must be subset of scanner intervals |

**Important:** v1.1 must not add a new `primaryTf` parameter to `openPosition()`. Current signature already has `signalInterval`; that is the primary timeframe.

---

## 8. Architecture

### 8.1 Chosen Approach: C++ Port + Python Module Subprocess

C++ owns the trading flow and calls a Python module only for Gemini analysis. Python handles SDK calls, chart rendering and scoring.

```text
SignalEngine::openPosition()
  -> calculate size
  -> ExposureController check
  -> GeminiFilter budget check
  -> GeminiFilterController.evaluate(symbol, direction, signalInterval, cache)
  -> if mode=enforce and decision=Block: return without order
  -> place market / TP / SL
```

**Why keep subprocess:**

- Avoids C++ Gemini SDK/client work.
- Keeps chart generation in Python where `plotly`, `kaleido`, and `pandas` provide maintained static chart rendering.
- Process isolation makes API/SDK failures easier to contain.

**v1.1 guardrail:** subprocess latency is budgeted. The design does not allow unlimited Gemini calls in one scan cycle.

### 8.2 Rejected Approaches

| Alternative | Rejection reason |
|---|---|
| FastAPI sidecar | Adds process lifecycle, healthcheck and restart complexity; reconsider only if latency/cost requires shared key state |
| Native C++ Gemini REST client | Higher maintenance cost; chart generation remains Python-heavy |
| Dynamic model selection via `list_models()` | Capability mismatch risk; pin models and fail explicitly |
| Global fail-closed by default | Too risky for first deploy; use shadow rollout first |

---

## 9. C++ Design

### 9.1 Config Types

```cpp
namespace engine {

enum class GeminiFilterMode {
    Disabled,
    Shadow,
    Enforce,
};

struct GeminiFilterConfig {
    bool enabled{false};
    GeminiFilterMode mode{GeminiFilterMode::Shadow};

    std::string pythonPath{"python"};
    std::string moduleName{"tools.gemini_filter.gemini_filter"};
    std::string workingDirectory{"."};
    std::string runtimeDir{"tmp/gemini_filter"};

    std::string sentimentModel{"gemini-3.1-pro-preview"};
    std::string visionModel{"gemini-3.1-pro-preview"};
    bool sentimentSearchThenScore{false};

    double sentimentWeight{0.5};
    double visionWeight{0.5};
    double confidenceThreshold{0.6};

    int timeoutSeconds{10};
    int maxEvaluationsPerScanCycle{3};
    int staleRuntimeTtlHours{24};

    std::vector<std::string> extraTfs{"1h", "4h"};
};

} // namespace engine
```

Validation at startup:

- `mode` must be one of `disabled`, `shadow`, `enforce`.
- `sentimentWeight >= 0`, `visionWeight >= 0`, and sum must be `> 0`.
- `confidenceThreshold` must be in `[0.0, 1.0]`.
- `timeoutSeconds` must be `> 0`; reject `0`.
- `maxEvaluationsPerScanCycle >= 0`; `0` means no Gemini calls and behavior follows budget-exhausted policy.
- Warn if any `extra_tfs` is absent from configured `scanner.intervals`.

### 9.2 Result Types

```cpp
namespace engine {

enum class GeminiDecision { Allow, Block };

struct GeminiFilterResult {
    GeminiDecision decision{GeminiDecision::Block};
    double confidence{0.0};
    double sentimentScore{0.0};
    double visionScore{0.0};
    std::string reason;
    std::string errorCode;
    bool hasError{false};
};

} // namespace engine
```

### 9.3 Port Interface

```cpp
namespace engine {

class IGeminiFilterPort {
public:
    virtual ~IGeminiFilterPort() = default;

    virtual GeminiFilterResult evaluate(
        std::string_view symbol,
        strategy::Signal::Direction direction,
        std::string_view signalInterval,
        const scanner::KlineCache& cache
    ) const = 0;
};

class NoOpGeminiFilterPort final : public IGeminiFilterPort {
public:
    GeminiFilterResult evaluate(
        std::string_view,
        strategy::Signal::Direction,
        std::string_view,
        const scanner::KlineCache&) const override {
        return {GeminiDecision::Allow, 1.0, 1.0, 1.0, "gemini filter disabled", {}, false};
    }
};

} // namespace engine
```

### 9.4 GeminiFilterController Responsibilities

`GeminiFilterController` is responsible for:

1. Snapshot primary and extra TF klines from `KlineCache`.
2. Create a UUID evaluation directory: `{runtimeDir}/eval-{uuid}/`.
3. Write `{evalDir}/input.json`.
4. Spawn Python with argument vector, not shell string: `python -m tools.gemini_filter.gemini_filter {evalDir}/input.json`.
5. Capture stdout and stderr separately.
6. Enforce `timeoutSeconds`; kill the subprocess/process tree on timeout.
7. Parse stdout JSON into `GeminiFilterResult`; malformed output => Block result.
8. Recursively delete `evalDir` in all normal/error paths.
9. On construction or first call, cleanup stale `eval-*` directories older than `staleRuntimeTtlHours`.

Do not log full prompt, raw API response, API keys, or file contents. Log only `eval_id`, scores, summary, latency and error code.

---

## 10. SignalEngine Integration

### 10.1 Constructor

```cpp
SignalEngine(
    IScannerPort& scanner,
    strategy::StrategyRegistry& registry,
    IAccountPort& account,
    IOrdersPort& orders,
    IExposurePort& exposure,
    IGeminiFilterPort& geminiFilter,
    GeminiFilterConfig geminiConfig,
    Config config);
```

Add members:

```cpp
IGeminiFilterPort& m_geminiFilter;
GeminiFilterConfig m_geminiConfig;
int m_geminiEvaluationsThisCycle{0};
```

### 10.2 Flow

```cpp
// Current call site already has item.interval:
co_await openPosition(item.symbol, item.interval, signal.direction, atr, currentPrice, cfg, signal.reason);
```

Inside `openPosition()` after ExposureController passes and before order placement:

```cpp
if (m_geminiConfig.enabled && m_geminiConfig.mode != GeminiFilterMode::Disabled) {
    if (m_geminiEvaluationsThisCycle >= m_geminiConfig.maxEvaluationsPerScanCycle) {
        if (m_geminiConfig.mode == GeminiFilterMode::Enforce) {
            Logger::instance().log(LogLevel::Warning, "gemini budget exhausted; blocking signal");
            co_return Result<void>{};
        }
        Logger::instance().log(LogLevel::Info, "gemini budget exhausted; shadow skip");
    } else {
        ++m_geminiEvaluationsThisCycle;
        const auto result = m_geminiFilter.evaluate(symbol, direction, signalInterval, m_scanner.cache());

        if (m_geminiConfig.mode == GeminiFilterMode::Shadow) {
            Logger::instance().log(LogLevel::Info, "gemini shadow decision=" + decisionToString(result.decision));
        } else if (result.decision == GeminiDecision::Block) {
            Logger::instance().log(LogLevel::Warning, "gemini blocked symbol=" + std::string(symbol));
            co_return Result<void>{};
        }
    }
}
```

Reset `m_geminiEvaluationsThisCycle = 0` at the start of every `runScanCycle()`.

### 10.3 Budget Policy

| Condition | `shadow` | `enforce` |
|---|---|---|
| Budget exhausted | Skip Gemini, allow order, log `shadow_skip_budget` | Block order |
| Timeout | Allow order, log `would_block_timeout` | Block order |
| Component API error | Allow order, log `would_block_component_error` | Block order |
| Confidence below threshold | Allow order, log `would_block_low_confidence` | Block order |

---

## 11. Subprocess Protocol

### 11.1 Command

C++ must call Python without shell interpolation:

```text
argv[0] = pythonPath
argv[1] = -m
argv[2] = moduleName
argv[3] = absolute_input_file_path
cwd     = workingDirectory
```

Default:

```text
python -m tools.gemini_filter.gemini_filter D:\...\tmp\gemini_filter\eval-<uuid>\input.json
```

### 11.2 Input JSON

```json
{
  "eval_id": "8e0e6bb5-9ca8-4e8a-b7f6-e0fdacbc0b69",
  "symbol": "BTCUSDT",
  "direction": "Long",
  "primary_tf": "4h",
  "extra_tfs": ["1h", "4h"],
  "klines": {
    "4h": [
      {
        "open_time": 1747440000000,
        "open": 96200.0,
        "high": 97100.0,
        "low": 95800.0,
        "close": 96850.0,
        "volume": 1234.5,
        "close_time": 1747454399999
      }
    ],
    "1h": []
  },
  "runtime_dir": "D:/.../tmp/gemini_filter/eval-<uuid>",
  "sentiment_model": "gemini-3.1-pro-preview",
  "vision_model": "gemini-3.1-pro-preview",
  "sentiment_search_then_score": false,
  "sentiment_weight": 0.5,
  "vision_weight": 0.5,
  "confidence_threshold": 0.6
}
```

Rules:

- Primary TF key must exist. If primary klines are empty, Python returns `Block` with `error_code="primary_ohlcv_missing"`.
- Extra TFs may be missing or empty; they are skipped without component failure.
- C++ should pass absolute paths for input file and runtime directory.

### 11.3 Output JSON

Successful evaluation:

```json
{
  "eval_id": "8e0e6bb5-9ca8-4e8a-b7f6-e0fdacbc0b69",
  "decision": "Allow",
  "confidence": 0.73,
  "sentiment_score": 0.80,
  "vision_score": 0.65,
  "sentiment_analysis": "News and broader crypto sentiment are favorable for the proposed long.",
  "vision_analysis": "Primary 4h chart confirms higher highs with controlled pullback.",
  "reason": "Sentiment and chart context confirm the long setup.",
  "error_code": null,
  "error": null,
  "latency_ms": 6420
}
```

Failure evaluation:

```json
{
  "eval_id": "8e0e6bb5-9ca8-4e8a-b7f6-e0fdacbc0b69",
  "decision": "Block",
  "confidence": 0.0,
  "sentiment_score": 0.0,
  "vision_score": 0.0,
  "sentiment_analysis": "",
  "vision_analysis": "",
  "reason": "Gemini sentiment component failed",
  "error_code": "sentiment_api_error",
  "error": "All Gemini keys failed for sentiment",
  "latency_ms": 10000
}
```

C++ treats missing fields, invalid JSON, non-finite numbers, score outside `[0,1]`, or unknown decision as `Block` result.

---

## 12. Python Package Design

### 12.1 Layout

```text
tools/gemini_filter/
  __init__.py
  gemini_filter.py        # module entry point: python -m tools.gemini_filter.gemini_filter
  key_manager.py          # key loading + random-start rotation
  gemini_client.py        # per-call google-genai client helpers and schemas
  chart_generator.py      # Plotly/Kaleido multi-TF chart PNG
  analyzer.py             # sentiment + vision orchestration
  requirements.txt
  tests/
    test_key_manager.py
    test_chart_generator.py
    test_analyzer.py
    test_entrypoint.py
```

### 12.2 Requirements

```text
google-genai>=1.0.0
plotly>=6.1.1
kaleido>=1.0.0
pandas>=2.2.0
python-dotenv>=1.0.0
```

Do not install or import `google-generativeai`.

### 12.3 Key Manager

```python
from __future__ import annotations

import os
import secrets
from dataclasses import dataclass
from typing import Callable, TypeVar

from google import genai
from google.genai import errors

T = TypeVar("T")

@dataclass(frozen=True)
class GeminiKey:
    name: str
    value: str

class GeminiKeyManager:
    def __init__(self) -> None:
        keys: list[GeminiKey] = []
        i = 1
        while True:
            value = os.getenv(f"GEMINI_API_KEY_{i}")
            if not value:
                break
            keys.append(GeminiKey(f"GEMINI_API_KEY_{i}", value))
            i += 1
        if not keys and os.getenv("GEMINI_API_KEY"):
            keys.append(GeminiKey("GEMINI_API_KEY", os.environ["GEMINI_API_KEY"]))
        if not keys and os.getenv("GEMINI_API_KEYS"):
            keys.extend(
                GeminiKey(f"GEMINI_API_KEYS[{idx}]", key)
                for idx, key in enumerate(split_key_list(os.environ["GEMINI_API_KEYS"]), start=1)
            )
        if not keys and os.getenv("GEMINI_TEXT_API_KEY"):
            keys.append(GeminiKey("GEMINI_TEXT_API_KEY", os.environ["GEMINI_TEXT_API_KEY"]))
        if not keys:
            raise RuntimeError("No Gemini API key found")

        self._keys = keys
        self._start = secrets.randbelow(len(keys))

    def run_with_rotation(self, fn: Callable[[genai.Client, GeminiKey], T]) -> T:
        last_error: Exception | None = None
        for offset in range(len(self._keys)):
            key = self._keys[(self._start + offset) % len(self._keys)]
            client = genai.Client(api_key=key.value)
            try:
                return fn(client, key)
            except Exception as exc:
                if not _is_retryable_key_error(exc):
                    raise
                last_error = exc
        raise RuntimeError(f"All {len(self._keys)} Gemini keys failed: {last_error}")

def _is_retryable_key_error(exc: Exception) -> bool:
    if isinstance(exc, errors.APIError):
        code = getattr(exc, "code", None) or getattr(exc, "status_code", None)
        return code in {401, 403, 429, 500, 502, 503, 504}
    msg = str(exc).lower()
    return any(token in msg for token in ["quota", "rate", "429", "resource_exhausted", "api key"])
```

Notes:

- No `genai.configure()`.
- No global SDK state.
- Random start avoids every subprocess hammering key 1.
- Key names may be logged; key values must never be logged.

### 12.4 Structured Output Schema

```python
SCORE_SCHEMA = {
    "type": "object",
    "properties": {
        "score": {
            "type": "number",
            "minimum": 0.0,
            "maximum": 1.0,
            "description": "Favorability score for the proposed trade direction."
        },
        "analysis": {
            "type": "string",
            "description": "Short explanation of the score."
        }
    },
    "required": ["score", "analysis"],
    "additionalProperties": False
}
```

Validation rules:

- Parse response as JSON only.
- Reject missing fields, non-numeric `score`, non-string `analysis`, NaN/Inf, or out-of-range values.
- Do not fallback to regex or neutral `0.5`.
- Parse/validation failure is a component error and overall `decision="Block"`.

### 12.5 Sentiment Analyzer

Preferred path (`sentiment_search_then_score=false`): one Gemini call with Google Search grounding and structured output.

```python
response = client.models.generate_content(
    model=sentiment_model,
    contents=prompt,
    config={
        "tools": [{"google_search": {}}],
        "response_format": {
            "text": {
                "mime_type": "application/json",
                "schema": SCORE_SCHEMA,
            }
        },
    },
)
```

Compatibility path (`sentiment_search_then_score=true`):

1. Call Gemini with Google Search grounding to get short evidence text with citations/grounding metadata.
2. Call Gemini without tools using structured output schema to convert evidence into `{score, analysis}`.

Use the compatibility path if the selected sentiment model or account does not support structured output combined with built-in tools.

### 12.6 Vision Analyzer

Vision uses structured output without Google Search tools:

```python
response = client.models.generate_content(
    model=vision_model,
    contents=[prompt, image_part],
    config={
        "response_format": {
            "text": {
                "mime_type": "application/json",
                "schema": SCORE_SCHEMA,
            }
        }
    },
)
```

Chart generation rules:

- Sanitize symbol and TF before using them in filenames.
- Write chart inside the evaluation directory only.
- Primary TF panel must be visually emphasized.
- If primary TF data is missing or empty, return component error.
- If extra TF data is missing, skip it and include that fact in analysis context.
- Export static PNG via Kaleido. Runtime hosts must have Kaleido's browser dependency available.

### 12.7 Parallel Execution

Sentiment and vision may run in two Python threads because both are I/O-bound. This is safe only because each thread creates its own `genai.Client` through `GeminiKeyManager.run_with_rotation()`.

Rules:

- Use a lock or `queue.Queue` to collect component results/errors.
- If either required component fails, final `decision="Block"`.
- If both components succeed, compute weighted confidence.
- Always attempt chart cleanup in Python `finally`; C++ remains the owner of final recursive cleanup.

### 12.8 Entry Point

Run only as module:

```bash
python -m tools.gemini_filter.gemini_filter <input_file_path>
```

`gemini_filter.py` responsibilities:

1. `load_dotenv()` from working directory.
2. Read input JSON.
3. Validate input schema.
4. Initialize `GeminiKeyManager`.
5. Run analyzer.
6. Print exactly one JSON object to stdout.
7. Send logs/debug/errors to stderr, not stdout.
8. Exit `0` if it successfully printed a valid Block/Allow JSON; exit non-zero only if it cannot print JSON at all.

---

## 13. Scoring

If both required components succeed:

```text
confidence = (sentiment_weight * sentiment_score + vision_weight * vision_score)
             / (sentiment_weight + vision_weight)

decision = Allow if confidence >= confidence_threshold else Block
```

If any required component fails:

```text
confidence = 0.0
decision = Block
error_code = <component_error>
```

Rationale: partial component failure must not silently become neutral or allow a trade in enforce mode. In shadow mode this still only logs `would_block`.

---

## 14. Configuration

### 14.1 config.json

```json
{
  "gemini_filter": {
    "enabled": false,
    "mode": "shadow",
    "python_path": "python",
    "module_name": "tools.gemini_filter.gemini_filter",
    "working_directory": ".",
    "runtime_dir": "tmp/gemini_filter",
    "sentiment_model": "gemini-3.1-pro-preview",
    "vision_model": "gemini-3.1-pro-preview",
    "sentiment_search_then_score": false,
    "sentiment_weight": 0.5,
    "vision_weight": 0.5,
    "confidence_threshold": 0.6,
    "timeout_seconds": 10,
    "max_evaluations_per_scan_cycle": 3,
    "stale_runtime_ttl_hours": 24,
    "extra_tfs": ["1h", "4h"]
  }
}
```

| Parameter | Default | Meaning |
|---|---|---|
| `enabled` | `false` | Hard off by default; uses `NoOpGeminiFilterPort` |
| `mode` | `shadow` | `shadow` evaluates/logs only; `enforce` can block |
| `python_path` | `python` | Python executable or venv path |
| `module_name` | `tools.gemini_filter.gemini_filter` | Module passed after `-m`; replaces script path |
| `working_directory` | `.` | CWD for Python process and `.env` loading |
| `runtime_dir` | `tmp/gemini_filter` | Base directory for UUID evaluation dirs |
| `sentiment_model` | `gemini-3.1-pro-preview` | Model for sentiment + Search + structured output path |
| `vision_model` | `gemini-3.1-pro-preview` | Model for chart image analysis |
| `sentiment_search_then_score` | `false` | Set `true` if selected model cannot combine Search + structured output |
| `sentiment_weight` | `0.5` | Weighted confidence component |
| `vision_weight` | `0.5` | Weighted confidence component |
| `confidence_threshold` | `0.6` | Minimum confidence to Allow |
| `timeout_seconds` | `45` | Hard subprocess timeout |
| `max_evaluations_per_scan_cycle` | `3` | Prevents unbounded scan-cycle delay |
| `stale_runtime_ttl_hours` | `24` | Cleanup TTL for abandoned eval dirs |
| `extra_tfs` | `["1h", "4h"]` | Extra chart context; must be in scanner intervals to have data |

### 14.2 .env

```bash
GEMINI_API_KEY_1=AIzaSy...key1...
GEMINI_API_KEY_2=AIzaSy...key2...
GEMINI_API_KEY_3=AIzaSy...key3...

# Single-key fallback if numbered keys are absent:
# GEMINI_API_KEY=AIzaSy...

# Legacy/local fallback supported by implementation:
# GEMINI_API_KEYS=AIzaSy...key1...,AIzaSy...key2...
# GEMINI_TEXT_API_KEY=AIzaSy...
```

---

## 15. Error Handling

| Scenario | Python behavior | C++ behavior |
|---|---|---|
| No Gemini key | Print Block JSON with `error_code="no_api_key"` | Shadow logs only; enforce blocks |
| One key quota/rate limited | Rotate to next key | N/A |
| All keys fail | Print Block JSON | Shadow logs only; enforce blocks |
| Subprocess timeout | May be killed before printing | Kill process, cleanup eval dir, synthesize Block result |
| Non-zero exit with no valid JSON | N/A | Synthesizes Block result with `subprocess_failed` |
| stdout malformed | N/A | Synthesizes Block result with `invalid_stdout_json` |
| stderr noisy but stdout valid | Logs stderr at debug/warn cap | Parse stdout normally |
| Primary OHLCV missing | Print Block JSON `primary_ohlcv_missing` | Shadow logs only; enforce blocks |
| Extra TF missing | Skip extra TF | Continue |
| Chart generation fail | Print Block JSON `chart_generation_failed` | Shadow logs only; enforce blocks |
| Structured output unsupported | Print Block JSON or use compatibility path if configured | Shadow logs only; enforce blocks |
| Score validation fail | Print Block JSON `invalid_model_output` | Shadow logs only; enforce blocks |
| Cleanup fails | Print warning to stderr | C++ attempts recursive cleanup and stale cleanup later |

---

## 16. Testing Strategy

### 16.1 C++ Unit Tests

| Test | Expected |
|---|---|
| `NoOpGeminiFilterPort` | Always Allow |
| Gemini Block in `shadow` | `orders.market()` is still called; log indicates would-block |
| Gemini Block in `enforce` | `orders.market()` is not called |
| Exposure Block before Gemini | Gemini mock is not called |
| Budget exhausted in `shadow` | Gemini mock not called; order proceeds |
| Budget exhausted in `enforce` | Gemini mock not called; order blocked |
| Existing `signalInterval` passed to Gemini | Mock observes interval equals `item.interval` |
| Invalid stdout parse | Controller returns Block result |
| Timeout path | Controller kills process and deletes eval dir |
| Unique temp paths | Concurrent/sequential evaluations do not collide |

### 16.2 Python Unit Tests

| Test | Expected |
|---|---|
| Entry point via `python -m` | Relative imports work; one JSON object on stdout |
| Direct script invocation | Either unsupported with clear error or covered by docs; not required path |
| Key loading numbered keys | Preserves all keys |
| Single-key fallback | Works with `GEMINI_API_KEY` |
| Legacy key fallback | Works with `GEMINI_API_KEYS` and `GEMINI_TEXT_API_KEY` |
| Random start index | Does not always start at key 1 |
| Rotation | Retryable key errors rotate; non-retryable errors propagate |
| No global SDK state | Tests assert `genai.configure` is not used |
| Structured output parser | Valid JSON accepted; invalid/missing/out-of-range rejected |
| No regex fallback | Prose-only response becomes component error |
| Chart generation | Creates Plotly/Kaleido PNG under eval dir |
| Missing primary data | Returns component error |
| Missing extra TF | Skips without component error |
| Analyzer component failure | Final decision Block |
| Cleanup after vision exception | Chart file removed best-effort |

### 16.3 Manual Smoke Tests

1. `python -m tools.gemini_filter.gemini_filter sample_input.json` with fake/mocked Gemini client.
2. Same command with real Gemini key and small OHLCV sample; verify stdout valid JSON and eval dir cleanup.
3. Bad key 1 + good key 2; verify rotation.
4. `enabled=true`, `mode="shadow"` on sandbox/testnet; verify `would_allow/would_block` logs and no behavioral block.
5. `mode="enforce"` with mock Block; verify no order is placed.
6. Timeout test with script sleeping beyond `timeout_seconds`; verify process kill and cleanup.

---

## 17. Phased Implementation Plan

### Phase A — Python Package

1. Add `tools/gemini_filter/requirements.txt` with `google-genai`.
2. Implement `key_manager.py` with client-per-key rotation.
3. Implement structured output schema and validation.
4. Implement `chart_generator.py` with eval-dir-only output and cleanup.
5. Implement `analyzer.py` sentiment + vision orchestration.
6. Implement module entry point `gemini_filter.py`.
7. Add Python unit tests.
8. Run module smoke test with mocked Gemini client.

### Phase B — C++ Controller

1. Add `src/engine/gemini_filter.h/.cpp`.
2. Implement config/result/interface/no-op/controller.
3. Implement UUID eval dir creation, input serialization, subprocess spawn, timeout, stdout/stderr capture and cleanup.
4. Add stale runtime cleanup.
5. Add C++ unit tests for controller parse/error/temp behavior.
6. Update `CMakeLists.txt`.

### Phase C — SignalEngine Integration

1. Add `IGeminiFilterPort& m_geminiFilter` and Gemini config/budget state.
2. Reset budget at start of `runScanCycle()`.
3. Call Gemini after exposure and before order placement.
4. Preserve current `signalInterval` parameter as primary TF.
5. Add `shadow` vs `enforce` tests.

### Phase D — Config/Wiring

1. Parse `gemini_filter` section in `main.cpp`.
2. Wire `NoOpGeminiFilterPort` when disabled; otherwise `GeminiFilterController`.
3. Warn when `extra_tfs` are absent from `scanner.intervals`.
4. Add `tmp/gemini_filter/` to `.gitignore`.
5. Document rollout steps in runbook if needed.

---

## 18. Decision Log

| Decision | Alternatives | Reason | Status |
|---|---|---|---|
| Python module subprocess | FastAPI sidecar, C++ REST client | Lowest implementation cost while isolating SDK/charting | Approved |
| Use `google-genai` | `google-generativeai` | Current official SDK path; avoids deprecated library | Approved |
| Invoke via `python -m` | Direct script path | Preserves package relative imports | Approved |
| Client-per-call/key | Global `genai.configure()` | Avoids thread races and key leakage across calls | Approved |
| Config-pinned models | `list_models()` auto-selection | Capability must be explicit and testable | Approved |
| Default disabled + shadow rollout | Default enforce | Avoids accidental global trade block on first deploy | Approved |
| Enforce fail-closed | Fail-open | Trading safety when AI gate is explicitly enforced | Approved |
| Budget per scan cycle | Unlimited Gemini calls | Prevents scan loop from accumulating unbounded AI latency | Approved |
| UUID eval dirs | Timestamp filenames | Prevents collision and simplifies cleanup | Approved |
| Structured output validation | Regex parse + neutral fallback | Invalid model output must not become neutral signal | Approved |
| Reuse `signalInterval` | Add new `primaryTf` parameter | Current code already carries primary timeframe | Approved |
| Random start key rotation per subprocess | Global round-robin state | Fixes key-1 hot spot without sidecar complexity | Approved |
| Guarded latest model resolver | Always pin, unguarded latest alias | Allows Google model updates without silently selecting non-trading model families | Approved |

---

## 19. References

- [Google GenAI SDK libraries](https://ai.google.dev/gemini-api/docs/libraries)
- [Grounding with Google Search](https://ai.google.dev/gemini-api/docs/google-search)
- [Structured outputs](https://ai.google.dev/gemini-api/docs/structured-output)

---

## 20. Latest Model Resolver Addendum

`model_resolver.py` may be enabled through `gemini_filter.model_resolution`.

Config shape:

```json
{
  "model_resolution": {
    "enabled": true,
    "mode": "latest_pro",
    "fallback_on_error": true,
    "allow_preview": true
  }
}
```

Supported modes:

- `pinned`: use `sentiment_model` and `vision_model` exactly as configured.
- `latest_pro`: call Gemini `models.list()`, filter core Pro models with `generateContent`, and choose the highest version.
- `latest_flash`: same selection logic for core Flash models.

Guardrails:

- Pinned `sentiment_model` and `vision_model` remain the fallback.
- Resolver excludes non-core families: image, audio/live, TTS, robotics, computer-use, embeddings and custom-tools variants.
- Resolver runs once before sentiment/vision threads start; both components use the same resolved model.
- If listing/filtering fails and `fallback_on_error=true`, log the failure and continue with pinned models.
- If `fallback_on_error=false`, resolver failure becomes a component error.

---

## 21. Gemini Failure Policy Addendum

Shadow mode only means "do not enforce ordinary model confidence decisions". It does not mean "fail open on infrastructure failures".

Config:

```json
{
  "block_on_error": true,
  "block_on_budget_exhausted": true
}
```

Rules:

- `decision=Block` with no `error_code` in `mode=shadow`: log `gemini shadow ...` and allow order placement.
- Any Gemini component error, including quota/rate-limit after key rotation, timeout, invalid JSON, missing key or analyzer exception: block when `block_on_error=true`.
- Internal budget exhaustion (`max_evaluations_per_scan_cycle` reached): block when `block_on_budget_exhausted=true`, even in shadow mode.
- Logs before Gemini gate use `strategy candidate signal` wording; only `opening position` means a signal passed all gates.

---

## 22. Shadow Mode Removal Addendum

`shadow` mode is removed from runtime behavior.

Current modes:

- `disabled`: Gemini filter is not called.
- `enforce`: Gemini is called and any `decision="Block"` blocks order placement.

Compatibility:

- If a stale config still uses `mode="shadow"`, startup logs a warning and forces `enforce`.

Locked behavior:

- Gemini `Block` without `error_code` blocks.
- Gemini component error/rate-limit/quota exhaustion after key rotation blocks when `block_on_error=true`.
- Gemini scan-cycle budget exhaustion blocks when `block_on_budget_exhausted=true`.
- Logs before Gemini gate use `strategy candidate signal`; only `opening position` means all gates passed.
