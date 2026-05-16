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


from datetime import datetime, timedelta
from zoneinfo import ZoneInfo
from unittest.mock import patch


def test_seconds_until_next_pacific_day_standard_night(tmp_path: Path) -> None:
    """On a regular night, result must match the real wall-clock distance to next midnight."""
    # 2025-01-15 23:00:00 PST (no DST active)
    fixed_now = datetime(2025, 1, 15, 23, 0, 0, tzinfo=ZoneInfo("America/Los_Angeles"))
    next_midnight = datetime(2025, 1, 16, 0, 0, 0, tzinfo=ZoneInfo("America/Los_Angeles"))
    expected = int((next_midnight - fixed_now).total_seconds())  # 3600

    import tools.gemini_filter.quota_manager as qm_module
    with patch.object(qm_module, "datetime") as mock_dt:
        mock_dt.now.return_value = fixed_now
        # Allow datetime constructor to still work for replace+timedelta
        mock_dt.side_effect = lambda *a, **kw: datetime(*a, **kw)
        result = QuotaManager._seconds_until_next_pacific_day()

    assert result == expected, f"Expected {expected}s, got {result}s"


def test_seconds_until_next_pacific_day_dst_spring_forward(tmp_path: Path) -> None:
    """During spring-forward (23-hour day), must still reach exactly next midnight.

    The old code does today_midnight.timestamp() + 86400, which on a spring-forward day
    overshoots to 01:00 PDT the next day instead of 00:00 PDT (midnight).
    The bug is visible from inside the PDT portion of the day (after 03:00 PDT).
    """
    # 2025-03-09 03:00:00 PDT — clocks sprang forward at 2:00 AM PST (now in PDT/UTC-7)
    # midnight of this day was at PST (UTC-8), but 'now' is PDT (UTC-7)
    # old: today_midnight(PST).timestamp() + 86400 = 2025-03-10 01:00 PDT (1h past midnight!)
    # new: today_midnight + timedelta(days=1) = 2025-03-10 00:00 PDT (correct)
    after_spring = datetime(2025, 3, 9, 3, 0, 0, tzinfo=ZoneInfo("America/Los_Angeles"))
    next_midnight = datetime(2025, 3, 10, 0, 0, 0, tzinfo=ZoneInfo("America/Los_Angeles"))
    expected = int((next_midnight - after_spring).total_seconds())  # 75600 (21h)

    import tools.gemini_filter.quota_manager as qm_module
    with patch.object(qm_module, "datetime") as mock_dt:
        mock_dt.now.return_value = after_spring
        mock_dt.side_effect = lambda *a, **kw: datetime(*a, **kw)
        result = QuotaManager._seconds_until_next_pacific_day()

    assert result == expected, f"Expected {expected}s (21h remaining on 23h spring-forward day), got {result}s"


def test_seconds_until_next_pacific_day_is_positive() -> None:
    """Result must always be >= 1 regardless of when it's called."""
    result = QuotaManager._seconds_until_next_pacific_day()
    assert result >= 1

