from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class RoutedModels:
    sentiment_candidates: list[str]
    vision_candidates: list[str]
    vision_pro_escalation_enabled: bool
    vision_pro_escalation_min_score: float
    vision_pro_escalation_max_score: float


def build_model_route(data: dict[str, Any], sentiment_model: str, vision_model: str) -> RoutedModels:
    raw = data.get("model_routing", {})
    if not isinstance(raw, dict) or not bool(raw.get("enabled", False)):
        return RoutedModels(
            sentiment_candidates=[sentiment_model],
            vision_candidates=[vision_model],
            vision_pro_escalation_enabled=False,
            vision_pro_escalation_min_score=0.45,
            vision_pro_escalation_max_score=0.65,
        )

    sentiment_raw = raw.get("sentiment", {})
    if not isinstance(sentiment_raw, dict):
        sentiment_raw = {}
    vision_raw = raw.get("vision", {})
    if not isinstance(vision_raw, dict):
        vision_raw = {}

    sentiment_candidates = _clean_candidates(sentiment_raw.get("candidates"), sentiment_model)
    vision_candidates = _clean_candidates(vision_raw.get("candidates"), vision_model)
    min_score = _safe_float(vision_raw.get("pro_escalation_min_score", 0.45))
    max_score = _safe_float(vision_raw.get("pro_escalation_max_score", 0.65))
    if min_score > max_score:
        min_score, max_score = max_score, min_score

    return RoutedModels(
        sentiment_candidates=sentiment_candidates,
        vision_candidates=vision_candidates,
        vision_pro_escalation_enabled=_safe_bool(vision_raw.get("pro_escalation_enabled", False)),
        vision_pro_escalation_min_score=max(0.0, min(1.0, min_score)),
        vision_pro_escalation_max_score=max(0.0, min(1.0, max_score)),
    )


def _clean_candidates(raw: Any, fallback: str) -> list[str]:
    if not isinstance(raw, list):
        return [fallback]
    candidates = [str(item).strip() for item in raw if str(item).strip()]
    if fallback not in candidates:
        candidates.append(fallback)
    return candidates


def _safe_float(value: object | None, default: float = 0.0) -> float:
    if value is None:
        return default
    if isinstance(value, float):
        return value
    try:
        s = str(value).strip()
        return float(s) if s else default
    except Exception:
        return default


def _safe_bool(value: object | None, default: bool = False) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    s = str(value).strip().lower()
    if s in {"1", "true", "yes", "y", "on"}:
        return True
    if s in {"0", "false", "no", "n", "off"}:
        return False
    return default

