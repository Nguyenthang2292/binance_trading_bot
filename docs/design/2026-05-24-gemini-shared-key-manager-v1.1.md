# Gemini Shared Key Manager - v1.1

**Version:** 1.1
**Date:** 2026-05-24
**Status:** APPROVED — ready for implementation
**Supersedes:** v1.0 (2026-05-24)
**Scope:** Extract `GeminiKeyManager` from `tools/gemini_filter/key_manager.py` into `tools/shared/gemini_key_manager.py` so that both `tools/gemini_filter` and `tools/backtest_range_proposer` share the same key-rotation infrastructure.

### Changes from v1.0

| Finding | Severity | Change |
|---------|----------|--------|
| P1a — `tests/python/test_key_manager.py` bị vỡ sau khi delete `gemini_filter/key_manager.py` | Critical | Thêm file test vào §3 target state và §7 affected files; cập nhật import path |
| P1b — `GeminiKeyManager._HTTP_TIMEOUT_MS=30s` > `BacktestGateGeminiConfig.timeoutSeconds=8s` → SDK timeout là dead code | Critical | D6 revise: timeout phải configurable; `_call_gemini()` truyền timeout xuống; §5.2 cập nhật `GeminiKeyManager` constructor; §5.7 cập nhật `_call_gemini()` |
| P2a — README chỉ đề cập `GEMINI_API_KEY`, không nói về multi-key vars | Medium | Thêm `tools/backtest_range_proposer/README.md` vào §7; §5.9 spec update README |
| P2b — `tools/backtest_range_proposer/requirements.txt` là dead metadata (không có install step) | Medium | D4 revise: xóa `requirements.txt` khỏi scope; doc note thay thế |

---

## 1. Summary

`tools/backtest_range_proposer/main.py` currently calls Gemini via `_call_gemini()`, a simplified helper that:

- Only reads a single `GEMINI_API_KEY` environment variable
- Creates `genai.Client` without an HTTP timeout
- Has no retry or key-rotation logic on quota/rate errors

Meanwhile `tools/gemini_filter` has a production-grade `GeminiKeyManager` that supports multi-key rotation (`GEMINI_API_KEY_1/2/...`), retries on 401/429/5xx, and a configurable HTTP timeout (default 30s).

This design extracts `GeminiKeyManager` into a new `tools/shared/` package, makes the HTTP timeout constructor-configurable, and wires both tools to import from the shared location. There is no shim — imports in `gemini_filter` are updated directly.

---

## 2. Goals & Non-Goals

### Goals

- Single source of truth for Gemini key management and retry logic
- `backtest_range_proposer` gains multi-key rotation with HTTP timeout aligned to its C++ outer deadline
- No behavioral change in `gemini_filter`
- `tests/python/test_key_manager.py` continues to pass after `gemini_filter/key_manager.py` is deleted
- Minimal diff: only import paths, `GeminiKeyManager` constructor, and `_call_gemini()` body change

### Non-Goals

- Dynamic model resolution for `backtest_range_proposer` (deferred)
- `response_schema` enforcement in `backtest_range_proposer` (deferred)
- Packaging / `pyproject.toml` at project root
- Pinning exact `google-genai` version

---

## 3. Codebase Context

### Current state

```
tools/
  gemini_filter/
    key_manager.py            ← GeminiKey + GeminiKeyManager (currently only used here)
    analyzer.py               ← from .key_manager import GeminiKey
    model_resolver.py         ← from .key_manager import GeminiKey
    gemini_filter.py          ← from .key_manager import GeminiKeyManager
  backtest_range_proposer/
    main.py                   ← _call_gemini(): single GEMINI_API_KEY, no rotation, no timeout
    README.md                 ← documents only GEMINI_API_KEY
tests/python/
  test_key_manager.py         ← imports from tools.gemini_filter.key_manager (line 7)
```

### Target state

```
tools/
  shared/
    __init__.py               ← NEW (empty package marker)
    gemini_key_manager.py     ← NEW (moved from gemini_filter/key_manager.py; timeout configurable)
  gemini_filter/
    key_manager.py            ← DELETED
    analyzer.py               ← EDIT: absolute import from tools.shared.gemini_key_manager
    model_resolver.py         ← EDIT: absolute import from tools.shared.gemini_key_manager
    gemini_filter.py          ← EDIT: absolute import from tools.shared.gemini_key_manager
  backtest_range_proposer/
    main.py                   ← EDIT: _call_gemini() uses GeminiKeyManager with aligned timeout
    README.md                 ← EDIT: document multi-key env vars
tests/python/
  test_key_manager.py         ← EDIT: import path → tools.shared.gemini_key_manager
```

---

## 4. Design Decisions

| ID | Decision | Alternatives Considered | Rationale |
|----|----------|-------------------------|-----------|
| D1 | Directory name `tools/shared/` | `tools/common/`, `tools/lib/`, `tools/core/` | Short, conventional, no ambiguity |
| D2 | No shim in `gemini_filter/key_manager.py` — delete and update imports directly | Keep shim re-exporting from shared | Shim uses absolute import inside relative-import package; if `tools/` not in `sys.path`, absolute import fails silently. Direct update eliminates the indirection entirely. |
| D3 | `GeminiKeyManager` instantiated per-call inside `_call_gemini()` | Module-level lazy singleton | Subprocess model: each C++ call spawns a new Python process → single call lifetime. Per-call init is simpler and easier to mock in tests. |
| D4 | No separate `requirements.txt` for `backtest_range_proposer` | Create new `requirements.txt` | `README.md` states tool reuses the same `.venv` as `gemini_filter`. No install script reads a separate file → it would be dead metadata. Dependencies documented in README instead. |
| D5 | `analyzer.py` and `model_resolver.py` switch to absolute imports | Keep relative imports via shim | Relative imports via shim create a two-hop resolution that confuses IDE navigation and static analysis. Absolute imports are explicit. |
| D6 | `GeminiKeyManager` accepts optional `http_timeout_ms: int` constructor param (default `30_000`) | Hardcode 30s globally | `BacktestGateGeminiConfig.timeoutSeconds = 8` (line 118, `backtest_gate.h`). C++ kills the subprocess at 8s. SDK timeout of 30s would **never fire** — and if it did, C++ would have already killed the process, preventing any retry. Setting SDK timeout < C++ outer timeout (e.g., 6 000 ms) ensures the SDK can raise a timeout exception and `run_with_rotation` can attempt the next key before C++ kills. |
| D7 | `_call_gemini()` derives `http_timeout_ms` from `timeoutSeconds` passed in payload's `budget` field | Hardcode 6s in `_call_gemini()` | `budget.timeout_seconds` is already present in `input.json` (set by C++ from `BacktestGateGeminiConfig.timeoutSeconds`). Reusing it makes `_call_gemini()` self-consistent without new contract. Set SDK timeout = `max(1, timeoutSeconds - 2) * 1000` ms to leave 2s margin for process overhead before C++ kill. |

---

## 5. File-by-File Specification

### 5.1 NEW `tools/shared/__init__.py`

```python
"""Shared utilities for tools/ subpackages."""
```

### 5.2 NEW `tools/shared/gemini_key_manager.py`

Content derived from `tools/gemini_filter/key_manager.py` with one change: `GeminiKeyManager.__init__` accepts an optional `http_timeout_ms` parameter.

```python
from __future__ import annotations

import os
import secrets
from dataclasses import dataclass
from typing import Callable, TypeVar

from google import genai
from google.genai import errors, types as genai_types

T = TypeVar("T")

_HTTP_TIMEOUT_MS = 30_000  # default; override via constructor


@dataclass(frozen=True)
class GeminiKey:
    name: str
    value: str


def _is_retryable_key_error(exc: Exception) -> bool:
    if isinstance(exc, errors.APIError):
        code = getattr(exc, "code", None) or getattr(exc, "status_code", None)
        return code in {401, 429, 500, 502, 503, 504}
    lowered = str(exc).lower()
    return any(
        token in lowered
        for token in ("quota", "rate", "429", "resource_exhausted", "api key", "unavailable")
    )


class GeminiKeyManager:
    def __init__(self, http_timeout_ms: int = _HTTP_TIMEOUT_MS) -> None:
        self._http_timeout_ms = http_timeout_ms
        keys: list[GeminiKey] = []
        index = 1
        while True:
            value = os.getenv(f"GEMINI_API_KEY_{index}")
            if not value:
                break
            keys.append(GeminiKey(name=f"GEMINI_API_KEY_{index}", value=value))
            index += 1

        single = os.getenv("GEMINI_API_KEY")
        if not keys and single:
            keys.append(GeminiKey(name="GEMINI_API_KEY", value=single))

        packed_keys = os.getenv("GEMINI_API_KEYS")
        if not keys and packed_keys:
            for packed_index, value in enumerate(_split_key_list(packed_keys), start=1):
                keys.append(GeminiKey(name=f"GEMINI_API_KEYS[{packed_index}]", value=value))

        text_key = os.getenv("GEMINI_TEXT_API_KEY")
        if not keys and text_key:
            keys.append(GeminiKey(name="GEMINI_TEXT_API_KEY", value=text_key))

        if not keys:
            raise RuntimeError("No Gemini API key found in environment")

        self._keys = keys
        self._start = secrets.randbelow(len(keys))

    def run_with_rotation(self, fn: Callable[[genai.Client, GeminiKey], T]) -> T:
        last_error: Exception | None = None
        for offset in range(len(self._keys)):
            key = self._keys[(self._start + offset) % len(self._keys)]
            client = genai.Client(
                api_key=key.value,
                http_options=genai_types.HttpOptions(timeout=self._http_timeout_ms),
            )
            try:
                return fn(client, key)
            except Exception as exc:
                if not _is_retryable_key_error(exc):
                    raise
                last_error = exc
        raise RuntimeError(f"All {len(self._keys)} Gemini keys failed: {last_error}")


def _split_key_list(raw: str) -> list[str]:
    normalized = raw.replace(";", ",").replace("\n", ",")
    return [part.strip() for part in normalized.split(",") if part.strip()]
```

**Backward compat note:** `_HTTP_TIMEOUT_MS` module constant is preserved so `test_key_manager.py`'s assertion `assert timeout == _HTTP_TIMEOUT_MS` still holds when `GeminiKeyManager()` is called with default constructor.

### 5.3 DELETED `tools/gemini_filter/key_manager.py`

Remove entirely. Replaced by updated imports in the three consumers below and the test file.

### 5.4 EDIT `tools/gemini_filter/analyzer.py`

```python
# Before
from .key_manager import GeminiKey

# After
from tools.shared.gemini_key_manager import GeminiKey
```

### 5.5 EDIT `tools/gemini_filter/model_resolver.py`

```python
# Before
from .key_manager import GeminiKey

# After
from tools.shared.gemini_key_manager import GeminiKey
```

### 5.6 EDIT `tools/gemini_filter/gemini_filter.py`

```python
# Before
from .key_manager import GeminiKeyManager

# After
from tools.shared.gemini_key_manager import GeminiKeyManager
```

### 5.7 EDIT `tools/backtest_range_proposer/main.py`

**Replace `_call_gemini()` (lines 194–220):**

```python
def _call_gemini(prompt: str, model: str, http_timeout_ms: int = 30_000) -> str:
    """Call Gemini API and return the raw text response."""
    try:
        from tools.shared.gemini_key_manager import GeminiKeyManager
        from google.genai import types  # type: ignore[import]
    except ImportError as exc:
        raise RuntimeError("google-genai or tools.shared package not available") from exc

    key_manager = GeminiKeyManager(http_timeout_ms=http_timeout_ms)

    def _do_call(client: Any, _key: Any) -> str:
        response = client.models.generate_content(
            model=model,
            contents=prompt,
            config=types.GenerateContentConfig(
                response_mime_type="application/json",
            ),
        )
        text = getattr(response, "text", None)
        if not isinstance(text, str) or not text.strip():
            raise RuntimeError("Gemini response contained no text")
        return text

    return key_manager.run_with_rotation(_do_call)
```

**In `main()` — derive `http_timeout_ms` from payload before calling Gemini:**

```python
# After _validate_input(payload), before _build_prompt():
model = payload.get("model", "gemini-2.0-flash")

# Derive SDK timeout from C++ outer timeout, leaving 2s margin for process overhead.
# BacktestGateGeminiConfig.timeoutSeconds default = 8 → http_timeout_ms = 6 000.
# If budget.timeout_seconds is absent, fall back to 28s (conservative for gemini_filter context).
_outer_s = int((payload.get("budget") or {}).get("timeout_seconds", 30))
_sdk_timeout_ms = max(1_000, (_outer_s - 2) * 1_000)

# In the _call_gemini() call:
raw_response = _call_gemini(prompt, model, http_timeout_ms=_sdk_timeout_ms)
```

Remove the old `api_key = os.environ.get("GEMINI_API_KEY")` guard — `GeminiKeyManager.__init__` raises `RuntimeError("No Gemini API key found in environment")` with the same semantics but covers all key env var variants.

**Note:** C++ must include `"timeout_seconds": timeoutSeconds` in the `budget` object of `input.json`. Verify `buildInputJson()` in `gemini_range_proposer.cpp` emits this field.

### 5.8 EDIT `tests/python/test_key_manager.py`

```python
# Before (line 7)
from tools.gemini_filter.key_manager import GeminiKeyManager, _is_retryable_key_error, _split_key_list, _HTTP_TIMEOUT_MS

# After
from tools.shared.gemini_key_manager import GeminiKeyManager, _is_retryable_key_error, _split_key_list, _HTTP_TIMEOUT_MS
```

No other changes to the test file — all test logic remains valid.

### 5.9 EDIT `tools/backtest_range_proposer/README.md`

Replace the "Required Environment Variable" section with:

```markdown
### Environment Variables

This tool uses the same Gemini key management as `tools/gemini_filter`.
Set one of the following in `.env` at the project root (or export in your shell):

| Variable | Description |
|----------|-------------|
| `GEMINI_API_KEY` | Single API key (simplest setup) |
| `GEMINI_API_KEY_1`, `GEMINI_API_KEY_2`, ... | Multiple keys — rotated automatically on quota/rate errors |
| `GEMINI_API_KEYS` | Comma- or semicolon-separated list of keys |
| `GEMINI_TEXT_API_KEY` | Fallback alias (lowest priority) |

At least one of the above must be set. Multi-key rotation improves resilience
against 429 quota errors during walk-forward optimization runs.
```

---

## 6. Import Path Assumption

All Python tools are invoked as:

```
python -m tools.<package>.<module> input.json output.json
```

from the project root directory (enforced by C++ subprocess via `chdir` before `execvp` / `CreateProcessA`). This means `tools.*` is always importable via `sys.path[0] = repo_root`. The absolute import `from tools.shared.gemini_key_manager import ...` relies on this pre-existing assumption — it is not a new constraint introduced by this design.

For test runners: ensure `PYTHONPATH=<repo_root>` or run tests from the repo root directory (standard practice). The existing `tests/python/conftest.py` already handles this.

---

## 7. Affected Files Summary

| File | Change |
|------|--------|
| `tools/shared/__init__.py` | CREATE |
| `tools/shared/gemini_key_manager.py` | CREATE (move + add `http_timeout_ms` param) |
| `tools/gemini_filter/key_manager.py` | DELETE |
| `tools/gemini_filter/analyzer.py` | EDIT — 1 import line |
| `tools/gemini_filter/model_resolver.py` | EDIT — 1 import line |
| `tools/gemini_filter/gemini_filter.py` | EDIT — 1 import line |
| `tools/backtest_range_proposer/main.py` | EDIT — `_call_gemini()` signature + body; timeout derivation in `main()` |
| `tools/backtest_range_proposer/README.md` | EDIT — env var section |
| `tests/python/test_key_manager.py` | EDIT — 1 import line (line 7) |

**C++ side (verify only — no change required if already present):**

| File | Action |
|------|--------|
| `src/backtest/gemini_range_proposer.cpp` — `buildInputJson()` | Verify `budget.timeout_seconds` is emitted in `input.json` |

---

## 8. Risks & Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| `tools/` not in `sys.path` in some test environment | Low | High | Existing `conftest.py` handles path setup; no new work needed |
| `GeminiKeyManager()` default constructor (no args) changes behavior for `gemini_filter` | None | — | Default `http_timeout_ms=30_000` matches existing `_HTTP_TIMEOUT_MS = 30_000`; no behavioral change |
| `budget.timeout_seconds` absent from `input.json` (C++ omits it) | Low | Medium | `_outer_s` falls back to `30` → SDK timeout = 28s; safe for `gemini_filter`-style invocations |
| SDK timeout fires and raises before C++ kill (desired behavior) | — | — | This is the correct path: Python raises exception → `run_with_rotation` retries next key → C++ kill is last resort |
| `test_client_constructed_with_http_timeout` expects `timeout == _HTTP_TIMEOUT_MS` | None | — | Default constructor unchanged; test still uses `GeminiKeyManager()` with no args → `_http_timeout_ms = 30_000 = _HTTP_TIMEOUT_MS` |

---

## 9. Multi-Agent Review Summary (v1.0) + Post-Review Findings (v1.1)

**Skeptic objections resolved (v1.0):**

- S1 (absolute import in shim) → eliminated shim entirely (D2)
- S2 (per-call GeminiKeyManager) → acceptable given subprocess model (D3)
- S3 (private re-export in shim) → moot, shim eliminated

**Constraint Guardian objections resolved (v1.0):**

- C1 (HTTP timeout) → D6 revised in v1.1: timeout now configurable
- C2/C3 (requirements.txt) → D4 revised in v1.1: no separate file

**User Advocate objections resolved (v1.0):**

- U1 (shim confusion) → direct import update, no shim (D2, D5)
- U2 (singleton mock difficulty) → per-call init kept (D3)

**Post-review findings addressed in v1.1:**

- P1a (test_key_manager.py vỡ) → §5.8, §7 added
- P1b (SDK timeout > C++ outer timeout → dead code) → D6 revised; D7 added; §5.2, §5.7 updated
- P2a (README env var docs lệch) → §5.9 added; §7 updated
- P2b (requirements.txt là dead metadata) → D4 revised; §3, §7 updated

**Arbiter verdict: APPROVED (v1.1)**

---

## 10. Out of Scope (Deferred)

- `response_schema` enforcement in `backtest_range_proposer` (constrain Gemini output at API level)
- Dynamic model resolution for `backtest_range_proposer` (auto-select `latest_flash` / `latest_pro`)
- Shared `tools/shared/gemini_client.py` for common `generate_json_score` / `generate_plain_text` helpers
