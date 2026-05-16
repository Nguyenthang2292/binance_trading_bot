from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from tools.gemini_filter.model_resolver import _latest_core_model, resolve_models


@dataclass
class _Model:
    name: str
    supported_actions: list[str]


class _FakeModels:
    def __init__(self, models: list[_Model]) -> None:
        self._models = models

    def list(self) -> list[_Model]:
        return self._models


class _FakeClient:
    def __init__(self, models: list[_Model]) -> None:
        self.models = _FakeModels(models)


class _FakeKeyManager:
    def __init__(self, models: list[_Model]) -> None:
        self._models = models

    def run_with_rotation(self, fn: Any) -> Any:
        return fn(_FakeClient(self._models), object())


def test_model_resolver_uses_pinned_when_disabled(tmp_path: Path) -> None:
    data = {
        "runtime_base_dir": str(tmp_path),
        "sentiment_model": "s-pinned",
        "vision_model": "v-pinned",
        "model_resolution": {"enabled": False},
    }
    resolved = resolve_models(data, _FakeKeyManager([]))
    assert resolved.sentiment_model == "s-pinned"
    assert resolved.vision_model == "v-pinned"


def test_model_resolver_latest_pro_and_cache_hit(tmp_path: Path) -> None:
    models = [
        _Model(name="models/gemini-3.0-flash", supported_actions=["generateContent"]),
        _Model(name="models/gemini-3.1-pro-preview", supported_actions=["generateContent"]),
    ]
    key_manager = _FakeKeyManager(models)
    data = {
        "runtime_base_dir": str(tmp_path),
        "sentiment_model": "s-pinned",
        "vision_model": "v-pinned",
        "model_resolution_ttl_seconds": 3600,
        "model_resolution_max_stale_seconds": 86400,
        "model_resolution": {
            "enabled": True,
            "mode": "latest_pro",
            "fallback_on_error": True,
            "allow_preview": True,
        },
    }
    first = resolve_models(data, key_manager)
    second = resolve_models(data, _FakeKeyManager([]))
    assert first.sentiment_model == "gemini-3.1-pro-preview"
    assert first.vision_model == "gemini-3.1-pro-preview"
    assert second.sentiment_model == "gemini-3.1-pro-preview"


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

