# Gemini Filter Bug Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix 5 confirmed bugs in `tools/gemini_filter/` and add regression tests that lock each fixed behaviour.

**Architecture:** Each fix is isolated to one or two files; tests go in the existing `tests/python/` directory alongside the module-specific test files. All tests use `pytest` with no external network calls — Google API interactions are replaced with simple fakes/mocks.

**Tech Stack:** Python 3.11+, pytest, `google-genai>=1.0.0`, `zoneinfo` (stdlib)

---

## File Map

| File | Change |
|------|--------|
| `tools/gemini_filter/gemini_client.py` | Fix #1: build config in one call; fix JSON+Search incompatibility |
| `tools/gemini_filter/model_resolver.py` | Fix #2: stable models preferred over preview |
| `tools/gemini_filter/key_manager.py` | Fix #3: HTTP timeout on Client; Fix #4: remove 403 from retryable |
| `tools/gemini_filter/quota_manager.py` | Fix #5: DST-safe midnight calculation |
| `tools/gemini_filter/analyzer.py` | Fix #3: add timeout to `join()` calls |
| `tests/python/test_gemini_client.py` | Tests for Fix #1 |
| `tests/python/test_model_resolver.py` | Tests for Fix #2 |
| `tests/python/test_key_manager.py` | Tests for Fix #3 + #4 |
| `tests/python/test_quota_manager.py` | Tests for Fix #5 |

---

## Task 1: Fix `gemini_client.py` — JSON schema + Google Search incompatibility

**Context:** `generate_json_score(use_google_search=True)` currently builds a config with
`response_mime_type="application/json"` then mutates `.tools` after construction. The Gemini API
rejects (or silently ignores) combining JSON schema output with Google Search grounding. When
`use_google_search=True`, the function must request plain text (so grounding works), then parse
the returned text as JSON. When `use_google_search=False`, use structured JSON schema output as
before.

**Files:**
- Modify: `tools/gemini_filter/gemini_client.py:57-76`
- Test: `tests/python/test_gemini_client.py`

- [ ] **Step 1: Write failing tests**

Append to `tests/python/test_gemini_client.py`:

```python
from unittest.mock import MagicMock, patch, call
from google.genai import types


def _make_fake_response(text: str) -> MagicMock:
    resp = MagicMock()
    resp.text = text
    return resp


def test_generate_json_score_without_search_uses_json_schema() -> None:
    """Config must use response_mime_type and response_schema, no tools."""
    fake_client = MagicMock()
    fake_client.models.generate_content.return_value = _make_fake_response(
        '{"score": 0.8, "analysis": "good"}'
    )
    from tools.gemini_filter.gemini_client import generate_json_score

    result = generate_json_score(
        client=fake_client,
        model="gemini-2.0-flash",
        contents="prompt",
        use_google_search=False,
    )
    assert result["score"] == 0.8
    config_used = fake_client.models.generate_content.call_args.kwargs["config"]
    assert config_used.response_mime_type == "application/json"
    assert config_used.tools is None or config_used.tools == []


def test_generate_json_score_with_search_omits_json_schema() -> None:
    """When search is enabled, config must NOT set response_mime_type=json (incompatible)."""
    fake_client = MagicMock()
    fake_client.models.generate_content.return_value = _make_fake_response(
        '{"score": 0.6, "analysis": "ok"}'
    )
    from tools.gemini_filter.gemini_client import generate_json_score

    result = generate_json_score(
        client=fake_client,
        model="gemini-2.0-flash",
        contents="prompt",
        use_google_search=True,
    )
    assert result["score"] == 0.6
    config_used = fake_client.models.generate_content.call_args.kwargs["config"]
    # JSON schema mode must be absent when grounding is active
    assert getattr(config_used, "response_mime_type", None) != "application/json"
    # Search tool must be present
    tools = getattr(config_used, "tools", None) or []
    assert len(tools) == 1


def test_generate_json_score_config_built_in_single_call() -> None:
    """Config object must not be mutated after construction (no post-init attribute assignment)."""
    configs_captured: list = []

    def fake_generate(**kwargs):  # type: ignore[override]
        configs_captured.append(kwargs["config"])
        resp = MagicMock()
        resp.text = '{"score": 0.5, "analysis": "x"}'
        return resp

    fake_client = MagicMock()
    fake_client.models.generate_content.side_effect = fake_generate
    from tools.gemini_filter.gemini_client import generate_json_score

    generate_json_score(
        client=fake_client,
        model="gemini-2.0-flash",
        contents="prompt",
        use_google_search=True,
    )
    assert len(configs_captured) == 1
    # The config passed must already have tools set (not None), proving no post-construction mutation
    cfg = configs_captured[0]
    assert (getattr(cfg, "tools", None) or [])  # non-empty tools list
```

- [ ] **Step 2: Run tests to verify they fail**

```
cd C:\Users\Admin\Desktop\binance_trading_bot
python -m pytest tests/python/test_gemini_client.py -v -k "search"
```
Expected: 2–3 FAILED (AssertionError or AttributeError on config inspection)

- [ ] **Step 3: Implement the fix**

Replace `generate_json_score` in `tools/gemini_filter/gemini_client.py`:

```python
def generate_json_score(
    *,
    client: Any,
    model: str,
    contents: Any,
    use_google_search: bool,
) -> dict[str, Any]:
    if use_google_search:
        # Google Search grounding is incompatible with JSON schema output mode.
        # Request plain text with grounding; the model is still prompted to return JSON.
        config = types.GenerateContentConfig(
            tools=[types.Tool(google_search=types.GoogleSearch())]
        )
    else:
        config = types.GenerateContentConfig(
            response_mime_type="application/json",
            response_schema=SCORE_SCHEMA,
        )
    response = client.models.generate_content(
        model=model,
        contents=contents,
        config=config,
    )
    return parse_score_text(_extract_text(response))
```

- [ ] **Step 4: Run tests to verify they pass**

```
python -m pytest tests/python/test_gemini_client.py -v
```
Expected: ALL PASS

- [ ] **Step 5: Commit**

```
git add tools/gemini_filter/gemini_client.py tests/python/test_gemini_client.py
git commit -m "fix(gemini_filter): build GenerateContentConfig in single call; fix JSON+Search incompatibility"
```

---

## Task 2: Fix `model_resolver.py` — stable models preferred over preview

**Context:** `preview_rank = 1 if match.group("preview") else 0` means `max()` picks preview over
stable when version numbers are equal. Invert to `0` for preview, `1` for stable.

**Files:**
- Modify: `tools/gemini_filter/model_resolver.py:70`
- Test: `tests/python/test_model_resolver.py`

- [ ] **Step 1: Write failing test**

Append to `tests/python/test_model_resolver.py`:

```python
from tools.gemini_filter.model_resolver import _latest_core_model


def test_stable_preferred_over_preview_same_version() -> None:
    """Stable model must win over preview when version numbers are identical."""
    models = [
        _Model(name="models/gemini-2.5-pro-preview", supported_actions=["generateContent"]),
        _Model(name="models/gemini-2.5-pro", supported_actions=["generateContent"]),
    ]
    result = _latest_core_model(models, tier="pro", allow_preview=True)
    assert result == "models/gemini-2.5-pro", f"Expected stable, got {result!r}"


def test_preview_still_returned_when_no_stable_exists() -> None:
    """If only preview models are available, return the preview."""
    models = [
        _Model(name="models/gemini-2.5-pro-preview", supported_actions=["generateContent"]),
    ]
    result = _latest_core_model(models, tier="pro", allow_preview=True)
    assert result == "models/gemini-2.5-pro-preview"


def test_preview_excluded_when_not_allowed() -> None:
    """When allow_preview=False, preview models must be ignored entirely."""
    models = [
        _Model(name="models/gemini-2.5-pro-preview", supported_actions=["generateContent"]),
        _Model(name="models/gemini-2.0-pro", supported_actions=["generateContent"]),
    ]
    result = _latest_core_model(models, tier="pro", allow_preview=False)
    assert result == "models/gemini-2.0-pro"
```

- [ ] **Step 2: Run test to verify it fails**

```
python -m pytest tests/python/test_model_resolver.py::test_stable_preferred_over_preview_same_version -v
```
Expected: FAILED — currently returns `"models/gemini-2.5-pro-preview"` instead of `"models/gemini-2.5-pro"`

- [ ] **Step 3: Implement the fix**

In `tools/gemini_filter/model_resolver.py`, change line 70:

```python
# Before:
preview_rank = 1 if match.group("preview") else 0
# After:
preview_rank = 0 if match.group("preview") else 1
```

- [ ] **Step 4: Run tests to verify they pass**

```
python -m pytest tests/python/test_model_resolver.py -v
```
Expected: ALL PASS

- [ ] **Step 5: Commit**

```
git add tools/gemini_filter/model_resolver.py tests/python/test_model_resolver.py
git commit -m "fix(gemini_filter): prefer stable models over preview when versions are equal"
```

---

## Task 3: Fix HTTP timeout + thread join timeout

**Context:** `genai.Client` is created with no `http_options`, so all API calls have no timeout.
A stalled network connection hangs the filter process forever because `t1.join()` / `t2.join()` in
`analyzer.py` also have no timeout. Fix both.

**Files:**
- Modify: `tools/gemini_filter/key_manager.py:65`
- Modify: `tools/gemini_filter/analyzer.py:490-491`
- Test: `tests/python/test_key_manager.py`

- [ ] **Step 1: Write failing test**

Append to `tests/python/test_key_manager.py`:

```python
import os
from unittest.mock import patch, MagicMock
from tools.gemini_filter.key_manager import GeminiKeyManager

_TIMEOUT_MS = 30_000


def test_client_constructed_with_http_timeout(monkeypatch: pytest.MonkeyPatch) -> None:
    """genai.Client must be constructed with an http_options timeout."""
    monkeypatch.setenv("GEMINI_API_KEY", "test-key")
    clients_created: list = []

    import google.genai as genai_module
    original_client = genai_module.Client

    def capturing_client(**kwargs):  # type: ignore[override]
        clients_created.append(kwargs)
        mock = MagicMock()
        mock.side_effect = None
        return mock

    monkeypatch.setattr(genai_module, "Client", capturing_client)

    km = GeminiKeyManager()

    def fake_fn(client, key):
        return "ok"

    km.run_with_rotation(fake_fn)
    assert clients_created, "Client was never constructed"
    kwargs = clients_created[0]
    http_options = kwargs.get("http_options")
    assert http_options is not None, "http_options not passed to genai.Client"
    timeout = getattr(http_options, "timeout", None)
    assert timeout is not None and timeout > 0, f"Expected positive timeout, got {timeout!r}"
```

- [ ] **Step 2: Run test to verify it fails**

```
python -m pytest tests/python/test_key_manager.py::test_client_constructed_with_http_timeout -v
```
Expected: FAILED — `http_options` is `None`

- [ ] **Step 3: Implement the fix — HTTP timeout in key_manager.py**

In `tools/gemini_filter/key_manager.py`, add the import and update `run_with_rotation`:

```python
from __future__ import annotations

import os
import secrets
from dataclasses import dataclass
from typing import Callable, TypeVar

from google import genai
from google.genai import errors, types as genai_types

T = TypeVar("T")

_HTTP_TIMEOUT_MS = 30_000  # 30 seconds


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
    def __init__(self) -> None:
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
                http_options=genai_types.HttpOptions(timeout=_HTTP_TIMEOUT_MS),
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

- [ ] **Step 4: Implement the fix — join timeout in analyzer.py**

In `tools/gemini_filter/analyzer.py`, replace lines 486–491:

```python
    t1 = threading.Thread(target=run_sentiment, daemon=True)
    t2 = threading.Thread(target=run_vision, daemon=True)
    t1.start()
    t2.start()
    t1.join(timeout=120.0)
    t2.join(timeout=120.0)
    if t1.is_alive() or t2.is_alive():
        with lock:
            errors.append("component_timeout:analysis threads did not finish within 120s")
```

- [ ] **Step 5: Run tests to verify they pass**

```
python -m pytest tests/python/test_key_manager.py -v
```
Expected: ALL PASS

- [ ] **Step 6: Commit**

```
git add tools/gemini_filter/key_manager.py tools/gemini_filter/analyzer.py tests/python/test_key_manager.py
git commit -m "fix(gemini_filter): add 30s HTTP timeout to genai.Client; add 120s thread join timeout"
```

---

## Task 4: Fix `key_manager.py` — remove 403 from retryable errors

**Context:** HTTP 403 (permission denied / model access not authorized) is a permanent error that
applies to the entire GCP project. Rotating keys from the same project will also get 403. Only
401 is worth rotating on (a specific key might be invalid). Remove 403 from the retryable set.

**Files:**
- Modify: `tools/gemini_filter/key_manager.py:21-23` (already fully replaced in Task 3 — confirm the change is there)
- Test: `tests/python/test_key_manager.py`

- [ ] **Step 1: Write failing tests**

Append to `tests/python/test_key_manager.py`:

```python
from google.genai import errors as genai_errors
from tools.gemini_filter.key_manager import _is_retryable_key_error


def _api_error(code: int) -> Exception:
    exc = MagicMock(spec=genai_errors.APIError)
    exc.code = code
    exc.status_code = None
    return exc


def test_403_is_not_retryable() -> None:
    assert not _is_retryable_key_error(_api_error(403))


def test_401_is_retryable() -> None:
    assert _is_retryable_key_error(_api_error(401))


def test_429_is_retryable() -> None:
    assert _is_retryable_key_error(_api_error(429))


def test_500_is_retryable() -> None:
    assert _is_retryable_key_error(_api_error(500))
```

- [ ] **Step 2: Run tests to verify the 403 test fails**

```
python -m pytest tests/python/test_key_manager.py::test_403_is_not_retryable -v
```
Expected: FAILED — 403 is currently retryable

- [ ] **Step 3: Verify the fix is in place from Task 3**

Check `tools/gemini_filter/key_manager.py` line ~23:

```python
return code in {401, 429, 500, 502, 503, 504}
```

`403` must NOT appear in this set. If Task 3 was done correctly, this is already fixed.

- [ ] **Step 4: Run tests to verify they pass**

```
python -m pytest tests/python/test_key_manager.py -v
```
Expected: ALL PASS

- [ ] **Step 5: Commit**

```
git add tests/python/test_key_manager.py
git commit -m "test(gemini_filter): lock _is_retryable_key_error behaviour; 403 must not rotate"
```

---

## Task 5: Fix `quota_manager.py` — DST-safe midnight calculation

**Context:** `now.replace(hour=0,...).timestamp() + 24*3600` adds exactly 86400 seconds, which is
wrong during DST transitions (Pacific timezone has 23-hour and 25-hour days). Use
`timedelta(days=1)` on a timezone-aware datetime instead.

**Files:**
- Modify: `tools/gemini_filter/quota_manager.py:1-10,137-141`
- Test: `tests/python/test_quota_manager.py`

- [ ] **Step 1: Write failing test**

Append to `tests/python/test_quota_manager.py`:

```python
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo
from tools.gemini_filter.quota_manager import QuotaManager

PACIFIC_TZ = ZoneInfo("America/Los_Angeles")


def test_seconds_until_next_pacific_day_near_dst_boundary() -> None:
    """During a DST spring-forward night the wait must still reach exactly next midnight."""
    # 2025-03-09 02:00 Pacific is when clocks spring forward; the day is only 23 hours.
    # One second before the clocks change: 2025-03-09 01:59:59 PST (UTC-8)
    before_dst = datetime(2025, 3, 9, 1, 59, 59, tzinfo=ZoneInfo("America/Los_Angeles"))
    next_midnight = datetime(2025, 3, 10, 0, 0, 0, tzinfo=ZoneInfo("America/Los_Angeles"))
    expected_seconds = int((next_midnight - before_dst).total_seconds())

    # Patch datetime.now inside quota_manager to return our fixed time
    import tools.gemini_filter.quota_manager as qm_module
    from unittest.mock import patch

    with patch.object(qm_module, "datetime") as mock_dt:
        mock_dt.now.return_value = before_dst
        mock_dt.side_effect = lambda *a, **kw: datetime(*a, **kw)
        result = QuotaManager._seconds_until_next_pacific_day()

    assert result == expected_seconds, (
        f"Expected {expected_seconds}s until next midnight, got {result}s"
    )


def test_seconds_until_next_pacific_day_is_positive() -> None:
    """Result must always be >= 1."""
    result = QuotaManager._seconds_until_next_pacific_day()
    assert result >= 1
```

- [ ] **Step 2: Run tests to verify they fail**

```
python -m pytest tests/python/test_quota_manager.py::test_seconds_until_next_pacific_day_near_dst_boundary -v
```
Expected: FAILED — result is off by up to 3600 seconds

- [ ] **Step 3: Implement the fix**

In `tools/gemini_filter/quota_manager.py`, add `timedelta` to imports and replace the method:

```python
# Top of file — add timedelta to the datetime import:
from datetime import datetime, timedelta

# Replace _seconds_until_next_pacific_day:
    @staticmethod
    def _seconds_until_next_pacific_day() -> int:
        now = datetime.now(PACIFIC_TZ)
        tomorrow = now.replace(hour=0, minute=0, second=0, microsecond=0) + timedelta(days=1)
        return int(max(1, math.ceil((tomorrow - now).total_seconds())))
```

- [ ] **Step 4: Run tests to verify they pass**

```
python -m pytest tests/python/test_quota_manager.py -v
```
Expected: ALL PASS

- [ ] **Step 5: Commit**

```
git add tools/gemini_filter/quota_manager.py tests/python/test_quota_manager.py
git commit -m "fix(gemini_filter): use timedelta(days=1) for DST-safe next-midnight calculation"
```

---

## Task 6: Full regression run

- [ ] **Step 1: Run all gemini_filter tests**

```
python -m pytest tests/python/ -v
```
Expected: ALL PASS. Note the count — it should have grown vs. the baseline.

- [ ] **Step 2: If any tests fail, diagnose and fix before final commit**

- [ ] **Step 3: Final commit (if any stragglers)**

```
git add -p
git commit -m "fix(gemini_filter): regression test cleanup"
```

---

## Self-Review

**Spec coverage:**
- Issue #1 (JSON+Search config) → Task 1 ✓
- Issue #2 (preview > stable) → Task 2 ✓
- Issue #3 (no HTTP timeout + no join timeout) → Task 3 ✓
- Issue #4 (403 retryable) → Task 4 ✓
- Issue #5 (DST midnight) → Task 5 ✓

**Placeholder scan:** No TBDs, all code blocks are complete.

**Type consistency:** `_Model`, `_FakeKeyManager`, `_FakeClient` defined in Task 2 tests match what already exists in `test_model_resolver.py`. `MagicMock`, `pytest` imported where needed.
