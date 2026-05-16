from __future__ import annotations

from tools.gemini_filter.model_router import build_model_route


def test_model_router_disabled_uses_pinned_models() -> None:
    route = build_model_route({}, "s-model", "v-model")
    assert route.sentiment_candidates == ["s-model"]
    assert route.vision_candidates == ["v-model"]
    assert not route.vision_pro_escalation_enabled


def test_model_router_enabled_uses_candidates_and_fallback() -> None:
    data = {
        "model_routing": {
            "enabled": True,
            "sentiment": {"candidates": ["m1", "m2"]},
            "vision": {
                "candidates": ["v1"],
                "pro_escalation_enabled": True,
                "pro_escalation_min_score": 0.7,
                "pro_escalation_max_score": 0.5,
            },
        }
    }
    route = build_model_route(data, "s-pinned", "v-pinned")
    assert route.sentiment_candidates == ["m1", "m2", "s-pinned"]
    assert route.vision_candidates == ["v1", "v-pinned"]
    assert route.vision_pro_escalation_enabled
    assert route.vision_pro_escalation_min_score <= route.vision_pro_escalation_max_score

