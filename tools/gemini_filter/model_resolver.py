from __future__ import annotations

import logging
import re
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast

from google import genai

from .cache_store import load_entry, save_entry
from .key_manager import GeminiKey

LOGGER = logging.getLogger("gemini_filter.model_resolver")

_CORE_MODEL_RE = re.compile(
    r"^models/gemini-(?P<version>\d+(?:\.\d+)?)-(?P<tier>pro|flash)(?P<lite>-lite)?(?P<preview>-preview)?$"
)


@dataclass(frozen=True)
class ResolvedModels:
    sentiment_model: str
    vision_model: str
    resolution: str


def _supported_actions(model: Any) -> list[str]:
    actions = getattr(model, "supported_actions", None)
    if actions is None:
        actions = getattr(model, "supported_generation_methods", None)
    if actions is None:
        return []
    return [str(action) for action in cast(list[Any], actions)]


def _version_key(version: str) -> tuple[int, ...]:
    return tuple(int(part) for part in version.split("."))


def _model_name(model: Any) -> str:
    return str(getattr(model, "name", "") or "")


def _is_generate_content_model(model: Any) -> bool:
    actions = _supported_actions(model)
    return not actions or "generateContent" in actions


def _list_models_with_rotation(client: genai.Client, _key: GeminiKey) -> list[Any]:
    return list(client.models.list())


def _latest_core_model(models: list[Any], *, tier: str, allow_preview: bool) -> str | None:
    candidates: list[tuple[tuple[int, ...], int, str]] = []
    for model in models:
        name = _model_name(model)
        match = _CORE_MODEL_RE.match(name)
        if not match:
            continue
        if match.group("tier") != tier:
            continue
        if match.group("lite"):
            continue
        if match.group("preview") and not allow_preview:
            continue
        if not _is_generate_content_model(model):
            continue
        preview_rank = 0 if match.group("preview") else 1
        candidates.append((_version_key(match.group("version")), preview_rank, name))
    if not candidates:
        return None
    return max(candidates)[2]


def resolve_models(data: dict[str, Any], key_manager: Any) -> ResolvedModels:
    raw_config = data.get("model_resolution", {})
    config: dict[str, Any]
    if isinstance(raw_config, dict):
        config = cast(dict[str, Any], raw_config)
    else:
        config = {}

    enabled = bool(config.get("enabled", False))
    mode = str(config.get("mode", "pinned")).strip().lower()
    fallback_on_error = bool(config.get("fallback_on_error", True))
    allow_preview = bool(config.get("allow_preview", True))

    pinned_sentiment = str(data["sentiment_model"])
    pinned_vision = str(data["vision_model"])
    if not enabled or mode in ("", "pinned", "disabled"):
        return ResolvedModels(pinned_sentiment, pinned_vision, "pinned")

    runtime_base = Path(str(data.get("runtime_base_dir") or data.get("runtime_dir") or "."))
    ttl_seconds = max(0, int(data.get("model_resolution_ttl_seconds", 3600)))
    max_stale_seconds = max(ttl_seconds, int(data.get("model_resolution_max_stale_seconds", 86400)))
    cache_key = (
        f"mode={mode}|allow_preview={allow_preview}|"
        f"pinned_sentiment={pinned_sentiment}|pinned_vision={pinned_vision}"
    )
    cached = load_entry(runtime_base / "cache", "model_resolution", cache_key)
    now = int(time.time())
    stale_cached: ResolvedModels | None = None
    if cached:
        cached_at = int(cached.get("cached_at", 0))
        age = max(0, now - cached_at)
        model_name = str(cached.get("model", ""))
        if model_name:
            resolved = ResolvedModels(model_name, model_name, "cache")
            if age <= ttl_seconds:
                LOGGER.info(
                    "model resolution cache hit mode=%s model=%s age_s=%d",
                    mode,
                    model_name,
                    age,
                )
                return resolved
            if age <= max_stale_seconds:
                stale_cached = resolved

    tier_by_mode = {
        "latest_pro": "pro",
        "latest_flash": "flash",
    }
    tier = tier_by_mode.get(mode)
    if not tier:
        message = f"unsupported model_resolution.mode={mode}"
        if not fallback_on_error:
            raise RuntimeError(message)
        LOGGER.warning("%s; fallback to pinned models", message)
        return ResolvedModels(pinned_sentiment, pinned_vision, "fallback_unsupported_mode")

    try:
        models = key_manager.run_with_rotation(_list_models_with_rotation)
        latest = _latest_core_model(models, tier=tier, allow_preview=allow_preview)
        if not latest:
            raise RuntimeError(f"no Gemini {tier} generateContent model found")
        resolved = latest.removeprefix("models/")
        save_entry(
            runtime_base / "cache",
            "model_resolution",
            cache_key,
            {
                "model": resolved,
                "cached_at": now,
                "mode": mode,
                "allow_preview": allow_preview,
            },
        )
        LOGGER.info(
            "model resolution completed mode=%s allow_preview=%s sentiment=%s vision=%s",
            mode,
            allow_preview,
            resolved,
            resolved,
        )
        return ResolvedModels(resolved, resolved, mode)
    except Exception:
        if not fallback_on_error:
            raise
        if stale_cached is not None:
            LOGGER.warning("model resolution failed; fallback to stale cache")
            return ResolvedModels(stale_cached.sentiment_model, stale_cached.vision_model, "cache_stale")
        LOGGER.exception("model resolution failed; fallback to pinned models")
        return ResolvedModels(pinned_sentiment, pinned_vision, "fallback_error")
