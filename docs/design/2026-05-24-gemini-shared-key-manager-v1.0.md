# Gemini Shared Key Manager - v1.0

**Version:** 1.0
**Date:** 2026-05-24
**Status:** APPROVED — ready for implementation
**Scope:** Extract `GeminiKeyManager` from `tools/gemini_filter/key_manager.py` into `tools/shared/gemini_key_manager.py` so that both `tools/gemini_filter` and `tools/backtest_range_proposer` share the same key-rotation infrastructure.

---

## 1. Summary

`tools/backtest_range_proposer/main.py` currently calls Gemini via `_call_gemini()`, a simplified helper that:
- Only reads a single `GEMINI_API_KEY` environment variable
- Creates `genai.Client` without an HTTP timeout
- Has no retry or key-rotation logic on quota/rate errors

Meanwhile `tools/gemini_filter` has a production-grade `GeminiKeyManager` that supports multi-key rotation (`GEMINI_API_KEY_1/2/...`), retries on 401/429/5xx, and a 30-second HTTP timeout.

This design extracts `GeminiKeyManager` (and its companion `GeminiKey` dataclass) into a new `tools/shared/` package, then wires both tools to import from the shared location. There is no shim or backward-compatibility layer — imports in `gemini_filter` are updated directly.

---

## 2. Goals & Non-Goals

### Goals

- Single source of truth for Gemini key management and retry logic
- `backtest_range_proposer` gains multi-key rotation and HTTP timeout for free
- No behavioral change in `gemini_filter`
- Minimal diff: only import paths and `_call_gemini()` body change

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
```

### Target state

```
tools/
  shared/
    __init__.py               ← NEW (empty package marker)
    gemini_key_manager.py     ← NEW (moved from gemini_filter/key_manager.py, unchanged)
  gemini_filter/
    key_manager.py            ← DELETED
    analyzer.py               ← EDIT: absolute import from tools.shared.gemini_key_manager
    model_resolver.py         ← EDIT: absolute import from tools.shared.gemini_key_manager
    gemini_filter.py          ← EDIT: absolute import from tools.shared.gemini_key_manager
  backtest_range_proposer/
    main.py                   ← EDIT: _call_gemini() uses GeminiKeyManager
    requirements.txt          ← NEW
```

---

## 4. Design Decisions

| ID | Decision | Alternatives Considered | Rationale |
|----|----------|-------------------------|-----------|
| D1 | Directory name `tools/shared/` | `tools/common/`, `tools/lib/`, `tools/core/` | Short, conventional, no ambiguity |
| D2 | No shim in `gemini_filter/key_manager.py` — delete and update imports directly | Keep shim re-exporting from shared | Shim uses absolute import inside relative-import package; if `tools/` not in `sys.path`, absolute import fails silently. Direct update eliminates the indirection entirely. |
| D3 | `GeminiKeyManager` instantiated per-call inside `_call_gemini()` | Module-level lazy singleton | Subprocess model: each C++ call spawns a new Python process → single call lifetime. Per-call init is simpler and easier to mock in tests. |
| D4 | `backtest_range_proposer/requirements.txt` lists `google-genai>=1.0.0` + `python-dotenv>=1.0.0` | Pin exact version | Mirrors constraint already in `gemini_filter/requirements.txt`; both tools run in the same `.venv` so version alignment is guaranteed at install time. |
| D5 | `analyzer.py` and `model_resolver.py` switch to absolute imports | Keep relative imports via shim | Relative imports via shim create a two-hop resolution that confuses IDE navigation and static analysis. Absolute imports are explicit. |
| D6 | HTTP timeout stays 30 000 ms (inherited from `GeminiKeyManager`) | Configurable per-tool timeout | C++ process kill acts as the hard outer timeout. 30s is generous for a single Gemini call. No behavioral change needed. |

---

## 5. File-by-File Specification

### 5.1 NEW `tools/shared/__init__.py`

```python
"""Shared utilities for tools/ subpackages."""
```

### 5.2 NEW `tools/shared/gemini_key_manager.py`

Content is **identical** to the current `tools/gemini_filter/key_manager.py`. No logic changes.

Public API:
- `GeminiKey` — frozen dataclass: `name: str`, `value: str`
- `GeminiKeyManager` — reads `GEMINI_API_KEY_1/2/...` → `GEMINI_API_KEY` → `GEMINI_API_KEYS` → `GEMINI_TEXT_API_KEY`; exposes `run_with_rotation(fn)` with retry on retryable errors
- `_is_retryable_key_error(exc)` — private, retryable on 401/429/500/502/503/504
- `_split_key_list(raw)` — private, splits comma/semicolon-separated key list

HTTP timeout: `_HTTP_TIMEOUT_MS = 30_000`

### 5.3 DELETED `tools/gemini_filter/key_manager.py`

Remove entirely. Replaced by updated imports in the three consumers below.

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

Replace `_call_gemini()` (lines 194–220) with:

```python
def _call_gemini(prompt: str, model: str) -> str:
    """Call Gemini API and return the raw text response."""
    try:
        from tools.shared.gemini_key_manager import GeminiKeyManager
        from google.genai import types  # type: ignore[import]
    except ImportError as exc:
        raise RuntimeError("google-genai or tools.shared package not available") from exc

    key_manager = GeminiKeyManager()

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

Remove the old `api_key = os.environ.get("GEMINI_API_KEY")` guard — `GeminiKeyManager.__init__` raises `RuntimeError("No Gemini API key found in environment")` with the same semantics but covers all key env var variants.

### 5.8 NEW `tools/backtest_range_proposer/requirements.txt`

```
google-genai>=1.0.0
python-dotenv>=1.0.0
```

---

## 6. Import Path Assumption

All Python tools are invoked as:

```
python -m tools.<package>.<module> input.json output.json
```

from the project root directory (enforced by C++ subprocess via `chdir` before `execvp` / `CreateProcessA`). This means `tools.*` is always importable via `sys.path[0] = repo_root`. The absolute import `from tools.shared.gemini_key_manager import ...` relies on this pre-existing assumption — it is not a new constraint introduced by this design.

For test runners: ensure `PYTHONPATH=<repo_root>` or run tests from the repo root directory (standard practice).

---

## 7. Affected Files Summary

| File | Change |
|------|--------|
| `tools/shared/__init__.py` | CREATE |
| `tools/shared/gemini_key_manager.py` | CREATE (move from gemini_filter/key_manager.py) |
| `tools/gemini_filter/key_manager.py` | DELETE |
| `tools/gemini_filter/analyzer.py` | EDIT — 1 import line |
| `tools/gemini_filter/model_resolver.py` | EDIT — 1 import line |
| `tools/gemini_filter/gemini_filter.py` | EDIT — 1 import line |
| `tools/backtest_range_proposer/main.py` | EDIT — `_call_gemini()` body (~27 lines) |
| `tools/backtest_range_proposer/requirements.txt` | CREATE |

---

## 8. Risks & Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| `tools/` not in `sys.path` in some test environment | Low | High | Document CWD assumption; add `conftest.py` with `sys.path` insert if needed |
| `GeminiKeyManager` constructor fails when only `GEMINI_API_KEY` set (single-key case) | None | — | Constructor already handles single-key fallback: `GEMINI_API_KEY_1` → `GEMINI_API_KEY` → ... |
| `backtest_range_proposer` test mocking is harder post-refactor | Low | Low | Mock `tools.shared.gemini_key_manager.GeminiKeyManager` directly; per-call init means `unittest.mock.patch` works cleanly |

---

## 9. Multi-Agent Review Summary

**Skeptic objections resolved:**
- S1 (absolute import in shim) → eliminated shim entirely (D2)
- S2 (per-call GeminiKeyManager) → acceptable given subprocess model (D3)
- S3 (private re-export in shim) → moot, shim eliminated

**Constraint Guardian objections resolved:**
- C1 (HTTP timeout) → 30s inherited from GeminiKeyManager; C++ kill is outer timeout (D6)
- C2/C3 (requirements.txt) → added to backtest_range_proposer (D4)

**User Advocate objections resolved:**
- U1 (shim confusion) → direct import update, no shim (D2, D5)
- U2 (singleton mock difficulty) → per-call init kept (D3)

**Arbiter verdict: APPROVED**

---

## 10. Out of Scope (Deferred)

- `response_schema` enforcement in `backtest_range_proposer` (would constrain Gemini output at API level, reducing need for manual validation — low priority)
- Dynamic model resolution for `backtest_range_proposer` (auto-select `latest_flash` / `latest_pro` via `client.models.list()`)
- Shared `tools/shared/gemini_client.py` for common `generate_json_score` / `generate_plain_text` helpers
