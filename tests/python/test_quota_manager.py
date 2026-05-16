from __future__ import annotations

from datetime import datetime
from pathlib import Path
from unittest.mock import patch
from zoneinfo import ZoneInfo

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


def _make_datetime_mock(fixed_now: datetime) -> type:
    """Return a datetime drop-in whose .now() returns fixed_now; constructor works normally."""

    class FakeDatetime(datetime):
        @classmethod
        def now(cls, tz=None):  # type: ignore[override]
            return fixed_now

    return FakeDatetime


def test_seconds_until_next_pacific_day_standard_night() -> None:
    """On a regular night, result must match the real wall-clock distance to next midnight."""
    fixed_now = datetime(2025, 1, 15, 23, 0, 0, tzinfo=ZoneInfo("America/Los_Angeles"))
    next_midnight = datetime(2025, 1, 16, 0, 0, 0, tzinfo=ZoneInfo("America/Los_Angeles"))
    expected = int((next_midnight - fixed_now).total_seconds())  # 3600

    import tools.gemini_filter.quota_manager as qm_module
    with patch.object(qm_module, "datetime", _make_datetime_mock(fixed_now)):
        result = QuotaManager._seconds_until_next_pacific_day()

    assert result == expected, f"Expected {expected}s, got {result}s"


def test_seconds_until_next_pacific_day_dst_spring_forward() -> None:
    """During spring-forward (23-hour day), must still reach exactly next midnight."""
    # 2025-03-09 03:00:00 PDT — after clocks spring forward (2:00 AM PST → 3:00 AM PDT)
    # This day is only 23 hours long; old code (+ 86400s) would overshoot by 1 hour.
    fixed_now = datetime(2025, 3, 9, 3, 0, 0, tzinfo=ZoneInfo("America/Los_Angeles"))
    next_midnight = datetime(2025, 3, 10, 0, 0, 0, tzinfo=ZoneInfo("America/Los_Angeles"))
    expected = int((next_midnight - fixed_now).total_seconds())  # 75600 (21h)

    import tools.gemini_filter.quota_manager as qm_module
    with patch.object(qm_module, "datetime", _make_datetime_mock(fixed_now)):
        result = QuotaManager._seconds_until_next_pacific_day()

    assert result == expected, f"Expected {expected}s (21h left in 23h day), got {result}s"


def test_seconds_until_next_pacific_day_dst_fall_back() -> None:
    """During fall-back (25-hour day), must still reach exactly next midnight."""
    # 2025-11-02 01:30:00 PDT — before clocks fall back (2:00 AM PDT → 1:00 AM PST)
    # This day is 25 hours long; old code (+ 86400s) would undershoot by 1 hour.
    fixed_now = datetime(2025, 11, 2, 1, 30, 0, tzinfo=ZoneInfo("America/Los_Angeles"))
    next_midnight = datetime(2025, 11, 3, 0, 0, 0, tzinfo=ZoneInfo("America/Los_Angeles"))
    expected = int((next_midnight - fixed_now).total_seconds())

    import tools.gemini_filter.quota_manager as qm_module
    with patch.object(qm_module, "datetime", _make_datetime_mock(fixed_now)):
        result = QuotaManager._seconds_until_next_pacific_day()

    assert result == expected, f"Expected {expected}s (in 25h day), got {result}s"


def test_seconds_until_next_pacific_day_is_positive() -> None:
    """Result must always be >= 1 regardless of when it's called."""
    result = QuotaManager._seconds_until_next_pacific_day()
    assert result >= 1

