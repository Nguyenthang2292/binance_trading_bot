from __future__ import annotations

from pathlib import Path

from tools.gemini_filter.metrics_store import MetricsStore
from tools.gemini_filter.quota_manager import QuotaConfig, QuotaManager


def test_quota_disabled_allows(tmp_path: Path) -> None:
    config = QuotaConfig(
        enabled=False,
        safety_factor=1.0,
        cooldown_seconds_on_429=10,
        default_rpm=1,
        default_rpd=1,
        model_limits={},
    )
    manager = QuotaManager(tmp_path, config, metrics_store=MetricsStore(tmp_path))
    result = manager.reserve("model-a")
    assert result.ok


def test_rpm_exhaustion_rejects_second_request(tmp_path: Path) -> None:
    config = QuotaConfig(
        enabled=True,
        safety_factor=1.0,
        cooldown_seconds_on_429=10,
        default_rpm=1,
        default_rpd=100,
        model_limits={},
    )
    manager = QuotaManager(tmp_path, config, metrics_store=MetricsStore(tmp_path))
    first = manager.reserve("model-a")
    second = manager.reserve("model-a")
    assert first.ok
    assert not second.ok
    assert second.reason in {"rpm exhausted", "model cooldown"}


def test_rpd_exhaustion_rejects_after_limit(tmp_path: Path) -> None:
    config = QuotaConfig(
        enabled=True,
        safety_factor=1.0,
        cooldown_seconds_on_429=10,
        default_rpm=100,
        default_rpd=1,
        model_limits={},
    )
    manager = QuotaManager(tmp_path, config, metrics_store=MetricsStore(tmp_path))
    first = manager.reserve("model-a")
    second = manager.reserve("model-a")
    assert first.ok
    assert not second.ok
    assert second.reason == "rpd exhausted"


def test_cooldown_blocks_reserve(tmp_path: Path) -> None:
    config = QuotaConfig(
        enabled=True,
        safety_factor=1.0,
        cooldown_seconds_on_429=10,
        default_rpm=100,
        default_rpd=100,
        model_limits={},
    )
    manager = QuotaManager(tmp_path, config, metrics_store=MetricsStore(tmp_path))
    manager.cooldown("model-a")
    result = manager.reserve("model-a")
    assert not result.ok
    assert result.reason == "model cooldown"

