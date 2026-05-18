from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from tools.gemini_filter import analyzer
from tools.gemini_filter.metrics_store import MetricsStore
from tools.gemini_filter.model_router import RoutedModels
from tools.gemini_filter.quota_manager import QuotaConfig, QuotaManager


class _KeyManager:
    def run_with_rotation(self, fn: Any) -> Any:
        return fn(object(), object())


def _base_data(tmp_path: Path) -> dict[str, Any]:
    return {
        "eval_id": "eval-1",
        "symbol": "BTCUSDT",
        "direction": "Long",
        "primary_tf": "1h",
        "klines": {"1h": [{"open_time": 1, "close_time": 2, "open": 1, "high": 2, "low": 1, "close": 2, "volume": 1}]},
        "runtime_dir": str(tmp_path / "runtime"),
        "runtime_base_dir": str(tmp_path / "runtime"),
        "sentiment_model": "s-model",
        "vision_model": "v-model",
        "sentiment_weight": 0.5,
        "vision_weight": 0.5,
        "confidence_threshold": 0.6,
        "sentiment_cache_ttl_seconds": 3600,
        "sentiment_cache_max_stale_seconds": 7200,
        "model_resolution": {"enabled": False},
        "quota": {"enabled": False},
    }


def test_sentiment_cache_hit_and_miss(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    data = _base_data(tmp_path)
    route = RoutedModels(
        sentiment_candidates=["m1"],
        vision_candidates=["v1"],
        vision_pro_escalation_enabled=False,
        vision_pro_escalation_min_score=0.45,
        vision_pro_escalation_max_score=0.65,
    )
    metrics = MetricsStore(Path(data["runtime_base_dir"]))
    quota = QuotaManager(
        Path(data["runtime_base_dir"]),
        QuotaConfig(
            enabled=False,
            safety_factor=1.0,
            cooldown_seconds_on_429=10,
            default_rpm=10,
            default_rpd=100,
            model_limits={},
        ),
        metrics_store=metrics,
    )

    calls = {"n": 0}

    def fake_route_call(**_: Any) -> tuple[dict[str, Any], str]:
        calls["n"] += 1
        return {"score": 0.7, "analysis": "ok"}, "m1"

    monkeypatch.setattr(analyzer, "_run_json_score_with_routes", fake_route_call)

    first = analyzer._analyze_sentiment(data, _KeyManager(), route, quota, metrics)
    second = analyzer._analyze_sentiment(data, _KeyManager(), route, quota, metrics)

    assert first["score"] == 0.7
    assert second["score"] == 0.7
    assert calls["n"] == 1

    snapshot = metrics.snapshot()
    assert snapshot["cache"]["sentiment"]["miss"] >= 1
    assert snapshot["cache"]["sentiment"]["hit"] >= 1


def test_sentiment_two_step_searches_then_scores_without_search(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    data = _base_data(tmp_path)
    data["sentiment_search_then_score"] = True
    route = RoutedModels(
        sentiment_candidates=["m1", "m2"],
        vision_candidates=["v1"],
        vision_pro_escalation_enabled=False,
        vision_pro_escalation_min_score=0.45,
        vision_pro_escalation_max_score=0.65,
    )
    metrics = MetricsStore(Path(data["runtime_base_dir"]))
    quota = QuotaManager(
        Path(data["runtime_base_dir"]),
        QuotaConfig(
            enabled=False,
            safety_factor=1.0,
            cooldown_seconds_on_429=10,
            default_rpm=10,
            default_rpd=100,
            model_limits={},
        ),
        metrics_store=metrics,
    )
    calls: list[dict[str, Any]] = []

    def fake_plain_text(**kwargs: Any) -> tuple[str, str]:
        calls.append({"kind": "plain", **kwargs})
        return "fresh market evidence", "m1"

    def fake_json_score(**kwargs: Any) -> tuple[dict[str, Any], str]:
        calls.append({"kind": "json", **kwargs})
        return {"score": 0.8, "analysis": "ok"}, "m2"

    monkeypatch.setattr(analyzer, "_run_plain_text_with_routes", fake_plain_text)
    monkeypatch.setattr(analyzer, "_run_json_score_with_routes", fake_json_score)

    result = analyzer._analyze_sentiment(data, _KeyManager(), route, quota, metrics)

    assert result["score"] == 0.8
    assert [call["kind"] for call in calls] == ["plain", "json"]
    assert calls[0]["component"] == "sentiment_evidence"
    assert calls[0]["use_google_search"] is True
    assert calls[1]["component"] == "sentiment_score"
    assert calls[1]["use_google_search"] is False
    assert "fresh market evidence" in calls[1]["contents"]


def test_sentiment_single_step_uses_legacy_search_json_path(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    data = _base_data(tmp_path)
    data["sentiment_search_then_score"] = False
    route = RoutedModels(
        sentiment_candidates=["m1"],
        vision_candidates=["v1"],
        vision_pro_escalation_enabled=False,
        vision_pro_escalation_min_score=0.45,
        vision_pro_escalation_max_score=0.65,
    )
    metrics = MetricsStore(Path(data["runtime_base_dir"]))
    quota = QuotaManager(
        Path(data["runtime_base_dir"]),
        QuotaConfig(
            enabled=False,
            safety_factor=1.0,
            cooldown_seconds_on_429=10,
            default_rpm=10,
            default_rpd=100,
            model_limits={},
        ),
        metrics_store=metrics,
    )
    calls: list[dict[str, Any]] = []

    def fake_json_score(**kwargs: Any) -> tuple[dict[str, Any], str]:
        calls.append(kwargs)
        return {"score": 0.7, "analysis": "ok"}, "m1"

    monkeypatch.setattr(analyzer, "_run_json_score_with_routes", fake_json_score)

    result = analyzer._analyze_sentiment(data, _KeyManager(), route, quota, metrics)

    assert result["score"] == 0.7
    assert len(calls) == 1
    assert calls[0]["component"] == "sentiment"
    assert calls[0]["use_google_search"] is True


def test_analyze_maps_quota_exhausted_error_code(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    data = _base_data(tmp_path)

    monkeypatch.setattr(
        analyzer,
        "resolve_models",
        lambda _data, _km: type("R", (), {"sentiment_model": "s", "vision_model": "v", "resolution": "pinned"})(),
    )
    monkeypatch.setattr(
        analyzer,
        "_analyze_sentiment",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(RuntimeError("quota_exhausted:sentiment")),
    )
    monkeypatch.setattr(
        analyzer,
        "_analyze_vision",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(RuntimeError("quota_exhausted:vision")),
    )

    result = analyzer.analyze(data, _KeyManager())
    assert result["decision"] == "Block"
    assert result["error_code"] == "quota_exhausted"


def test_analyze_allow_when_confidence_above_threshold(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    data = _base_data(tmp_path)
    data["confidence_threshold"] = 0.6

    monkeypatch.setattr(
        analyzer,
        "resolve_models",
        lambda _data, _km: type("R", (), {"sentiment_model": "s", "vision_model": "v", "resolution": "pinned"})(),
    )
    monkeypatch.setattr(analyzer, "_analyze_sentiment", lambda *_args, **_kwargs: {"score": 0.9, "analysis": "good"})
    monkeypatch.setattr(analyzer, "_analyze_vision", lambda *_args, **_kwargs: {"score": 0.9, "analysis": "good"})

    result = analyzer.analyze(data, _KeyManager())
    assert result["decision"] == "Allow"
    assert result["error_code"] is None

