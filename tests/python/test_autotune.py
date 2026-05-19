from __future__ import annotations

from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import time
from typing import Any

import pytest

from tools.gemini_filter import autotune


def _bucket_name(ts: datetime) -> str:
    return ts.strftime("%Y-%m-%dT%H-%M.json")


def _write_bucket(
    runtime_base: Path,
    *,
    minutes_ago: int,
    quota: dict[str, Any] | None = None,
    analyzer_data: dict[str, Any] | None = None,
    decisions: dict[str, int] | None = None,
) -> Path:
    bucket_dir = runtime_base / "cache" / "metrics" / "buckets"
    bucket_dir.mkdir(parents=True, exist_ok=True)
    ts = datetime.now(timezone.utc).replace(second=0, microsecond=0) - timedelta(minutes=minutes_ago)
    payload = {
        "schema_version": 1,
        "bucket_start_utc": ts.isoformat().replace("+00:00", "Z"),
        "cache": {},
        "quota": quota or {},
        "analyzer": analyzer_data or {},
        "decisions": decisions or {"allow": 0, "block": 0, "quota_exhausted": 0, "component_error": 0},
    }
    path = bucket_dir / _bucket_name(ts)
    path.write_text(json.dumps(payload, ensure_ascii=True), encoding="utf-8")
    return path


def _write_config(path: Path, runtime_base: Path, mode: str, **overrides: Any) -> None:
    autotune_cfg = {
        "enabled": True,
        "mode": mode,
        "interval_seconds": 900,
        "override_ttl_seconds": 1800,
        "min_samples_per_model": 1,
        "quota_pressure_demote_threshold": 0.4,
        "success_rate_promote_threshold": 0.95,
        "latency_promote_improvement_pct": 20,
        "max_block_rate_increase_pct": 10,
        "block_rate_window_seconds": 300,
        "bucket_retention_seconds": 90000,
        "emergency_quota_exhausted_cycles": 3,
        "weights": {
            "reserve_success_rate": 0.45,
            "quota_pressure": 0.30,
            "normalized_latency": 0.15,
            "decision_quality_proxy": 0.10,
        },
    }
    autotune_cfg.update(overrides)
    payload = {
        "gemini_filter": {
            "runtime_dir": str(runtime_base),
            "sentiment_model": "gemini-2.5-flash-lite",
            "vision_model": "gemini-3.1-flash-lite",
            "model_routing": {
                "enabled": True,
                "sentiment": {"candidates": ["gemini-2.5-flash-lite", "gemini-2.5-flash", "gemini-3.1-pro-preview"]},
                "vision": {
                    "candidates": ["gemini-3.1-flash-lite", "gemini-2.5-flash", "gemini-3.1-pro-preview"],
                    "pro_escalation_enabled": True,
                    "pro_escalation_min_score": 0.45,
                    "pro_escalation_max_score": 0.65,
                },
            },
            "quota": {
                "enabled": True,
                "safety_factor": 0.7,
                "models": {
                    "gemini-2.5-flash-lite": {"rpm": 20, "rpd": 1000},
                    "gemini-2.5-flash": {"rpm": 10, "rpd": 500},
                    "gemini-3.1-flash-lite": {"rpm": 10, "rpd": 500},
                    "gemini-3.1-pro-preview": {"rpm": 2, "rpd": 50},
                },
            },
            "autotune": autotune_cfg,
        }
    }
    path.write_text(json.dumps(payload, ensure_ascii=True), encoding="utf-8")


def _seed_default_bucket(runtime_base: Path, *, minutes_ago: int = 0) -> None:
    _write_bucket(
        runtime_base,
        minutes_ago=minutes_ago,
        quota={
            "gemini-2.5-flash-lite": {
                "reserve_ok": 10,
                "reserve_reject_rpm": 0,
                "reserve_reject_rpd": 0,
                "reserve_reject_cooldown": 0,
                "cooldown_set": 0,
            },
            "gemini-2.5-flash": {
                "reserve_ok": 9,
                "reserve_reject_rpm": 1,
                "reserve_reject_rpd": 0,
                "reserve_reject_cooldown": 0,
                "cooldown_set": 0,
            },
        },
        analyzer_data={
            "gemini-2.5-flash-lite": {
                "sentiment_calls": 10,
                "vision_calls": 0,
                "latency_stats": {
                    "count": 10,
                    "sum_ms": 2000,
                    "min_ms": 150,
                    "max_ms": 300,
                    "histogram_bucket_ms": 100,
                    "histogram": [0, 2, 8] + [0] * 48,
                },
            },
            "gemini-2.5-flash": {
                "sentiment_calls": 10,
                "vision_calls": 0,
                "latency_stats": {
                    "count": 10,
                    "sum_ms": 3000,
                    "min_ms": 200,
                    "max_ms": 400,
                    "histogram_bucket_ms": 100,
                    "histogram": [0, 0, 5, 5] + [0] * 47,
                },
            },
        },
        decisions={"allow": 8, "block": 2, "quota_exhausted": 0, "component_error": 0},
    )


def _read_override(runtime_base: Path, name: str) -> dict[str, Any]:
    return json.loads((runtime_base / "cache" / "autotune" / name).read_text(encoding="utf-8"))


def test_autotune_observe_writes_recommendation_only(tmp_path: Path, caplog: pytest.LogCaptureFixture) -> None:
    runtime_base = tmp_path / "runtime"
    _seed_default_bucket(runtime_base)
    config_path = tmp_path / "config.json"
    _write_config(config_path, runtime_base, "observe")

    caplog.set_level("INFO")
    rc = autotune.run(config_path, runtime_base)
    assert rc == 0
    assert (runtime_base / "cache" / "autotune" / "last_recommendation.json").exists()
    assert not (runtime_base / "cache" / "autotune" / "active_override.json").exists()
    assert "autotune recommendation mode=observe" in caplog.text


def test_autotune_apply_writes_active_override(tmp_path: Path) -> None:
    runtime_base = tmp_path / "runtime"
    _seed_default_bucket(runtime_base)
    config_path = tmp_path / "config.json"
    _write_config(config_path, runtime_base, "apply")

    rc = autotune.run(config_path, runtime_base)
    assert rc == 0
    assert (runtime_base / "cache" / "autotune" / "last_recommendation.json").exists()
    assert (runtime_base / "cache" / "autotune" / "active_override.json").exists()


def test_emergency_triggers_at_threshold_not_after(tmp_path: Path) -> None:
    runtime_base = tmp_path / "runtime"
    _seed_default_bucket(runtime_base, minutes_ago=0)
    _write_bucket(runtime_base, minutes_ago=1, decisions={"allow": 0, "block": 1, "quota_exhausted": 1, "component_error": 0})
    state_path = runtime_base / "cache" / "autotune" / "state.json"
    state_path.parent.mkdir(parents=True, exist_ok=True)
    state_path.write_text(
        json.dumps({"schema_version": 1, "consecutive_quota_exhausted_cycles": 2}, ensure_ascii=True),
        encoding="utf-8",
    )
    config_path = tmp_path / "config.json"
    _write_config(config_path, runtime_base, "apply", emergency_quota_exhausted_cycles=3)

    rc = autotune.run(config_path, runtime_base)
    assert rc == 0
    active = _read_override(runtime_base, "active_override.json")
    assert active["override_type"] == "emergency"


def test_dynamic_quota_never_exceeds_static_limits(tmp_path: Path) -> None:
    runtime_base = tmp_path / "runtime"
    _seed_default_bucket(runtime_base)
    config_path = tmp_path / "config.json"
    _write_config(config_path, runtime_base, "apply")

    autotune.run(config_path, runtime_base)
    active = _read_override(runtime_base, "active_override.json")
    static_cfg = json.loads(config_path.read_text(encoding="utf-8"))["gemini_filter"]["quota"]["models"]
    for model, limits in active["quota_override"]["models"].items():
        assert limits["rpm"] <= static_cfg[model]["rpm"]
        assert limits["rpd"] <= static_cfg[model]["rpd"]


def test_demote_under_pressure(tmp_path: Path) -> None:
    runtime_base = tmp_path / "runtime"
    _write_bucket(
        runtime_base,
        minutes_ago=0,
        quota={
            "gemini-2.5-flash-lite": {
                "reserve_ok": 2,
                "reserve_reject_rpm": 5,
                "reserve_reject_rpd": 0,
                "reserve_reject_cooldown": 0,
                "cooldown_set": 0,
            },
            "gemini-2.5-flash": {
                "reserve_ok": 8,
                "reserve_reject_rpm": 0,
                "reserve_reject_rpd": 0,
                "reserve_reject_cooldown": 0,
                "cooldown_set": 0,
            },
        },
    )
    config_path = tmp_path / "config.json"
    _write_config(config_path, runtime_base, "apply")
    autotune.run(config_path, runtime_base)
    active = _read_override(runtime_base, "active_override.json")
    sent = active["model_routing_override"]["sentiment"]["candidates"]
    assert sent.index("gemini-2.5-flash") < sent.index("gemini-2.5-flash-lite")


def test_insufficient_samples_keeps_static_order(tmp_path: Path) -> None:
    runtime_base = tmp_path / "runtime"
    _write_bucket(
        runtime_base,
        minutes_ago=0,
        quota={
            "gemini-2.5-flash-lite": {
                "reserve_ok": 1,
                "reserve_reject_rpm": 0,
                "reserve_reject_rpd": 0,
                "reserve_reject_cooldown": 0,
                "cooldown_set": 0,
            },
            "gemini-2.5-flash": {
                "reserve_ok": 1,
                "reserve_reject_rpm": 0,
                "reserve_reject_rpd": 0,
                "reserve_reject_cooldown": 0,
                "cooldown_set": 0,
            },
        },
    )
    config_path = tmp_path / "config.json"
    _write_config(config_path, runtime_base, "apply", min_samples_per_model=10)
    autotune.run(config_path, runtime_base)
    active = _read_override(runtime_base, "active_override.json")
    sent = active["model_routing_override"]["sentiment"]["candidates"]
    assert sent[:2] == ["gemini-2.5-flash-lite", "gemini-2.5-flash"]


def test_rollback_trigger(tmp_path: Path) -> None:
    runtime_base = tmp_path / "runtime"
    _seed_default_bucket(runtime_base)
    active_path = runtime_base / "cache" / "autotune" / "active_override.json"
    active_path.parent.mkdir(parents=True, exist_ok=True)
    active_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "generated_at": "2026-05-18T10:00:00Z",
                "expiry_at": "2099-01-01T00:00:00Z",
                "override_type": "normal",
            },
            ensure_ascii=True,
        ),
        encoding="utf-8",
    )
    state_path = runtime_base / "cache" / "autotune" / "state.json"
    state_path.write_text(
        json.dumps({"schema_version": 1, "active_override_baseline": {"block_rate_24h_before_apply": 0.05}}, ensure_ascii=True),
        encoding="utf-8",
    )
    _write_bucket(runtime_base, minutes_ago=0, decisions={"allow": 1, "block": 9, "quota_exhausted": 0, "component_error": 0})
    config_path = tmp_path / "config.json"
    _write_config(config_path, runtime_base, "apply", max_block_rate_increase_pct=10)

    autotune.run(config_path, runtime_base)
    active = _read_override(runtime_base, "active_override.json")
    assert active["override_type"] == "rollback"


def test_emergency_has_priority_over_rollback(tmp_path: Path) -> None:
    runtime_base = tmp_path / "runtime"
    _seed_default_bucket(runtime_base)
    _write_bucket(runtime_base, minutes_ago=0, decisions={"allow": 1, "block": 9, "quota_exhausted": 1, "component_error": 0})
    active_path = runtime_base / "cache" / "autotune" / "active_override.json"
    active_path.parent.mkdir(parents=True, exist_ok=True)
    active_path.write_text(json.dumps({"schema_version": 1, "generated_at": "2026-05-18T10:00:00Z"}, ensure_ascii=True), encoding="utf-8")
    state_path = runtime_base / "cache" / "autotune" / "state.json"
    state_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "consecutive_quota_exhausted_cycles": 2,
                "active_override_baseline": {"block_rate_24h_before_apply": 0.05},
            },
            ensure_ascii=True,
        ),
        encoding="utf-8",
    )
    config_path = tmp_path / "config.json"
    _write_config(config_path, runtime_base, "apply", emergency_quota_exhausted_cycles=3)
    autotune.run(config_path, runtime_base)
    active = _read_override(runtime_base, "active_override.json")
    assert active["override_type"] == "emergency"
    reasons = " ".join(active.get("reason_summary", []))
    assert "rollback signal ignored" in reasons


def test_cold_start_without_buckets_still_outputs_recommendation(tmp_path: Path) -> None:
    runtime_base = tmp_path / "runtime"
    config_path = tmp_path / "config.json"
    _write_config(config_path, runtime_base, "observe")
    rc = autotune.run(config_path, runtime_base)
    assert rc == 0
    assert (runtime_base / "cache" / "autotune" / "last_recommendation.json").exists()


def test_cleanup_retains_current_bucket_and_removes_old_and_tmp(tmp_path: Path) -> None:
    runtime_base = tmp_path / "runtime"
    _seed_default_bucket(runtime_base, minutes_ago=0)
    old_path = _write_bucket(runtime_base, minutes_ago=2000)
    tmp_path_file = runtime_base / "cache" / "metrics" / "buckets" / "stale.json.tmp"
    tmp_path_file.write_text("{}", encoding="utf-8")
    old_mtime = time.time() - 200000
    import os as _os
    _os.utime(old_path, (old_mtime, old_mtime))
    _os.utime(tmp_path_file, (old_mtime, old_mtime))
    config_path = tmp_path / "config.json"
    _write_config(config_path, runtime_base, "observe", bucket_retention_seconds=3600)
    autotune.run(config_path, runtime_base)
    assert not old_path.exists()
    assert not tmp_path_file.exists()
    current_files = list((runtime_base / "cache" / "metrics" / "buckets").glob("*.json"))
    assert current_files


def test_atomic_write_calls_fsync(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    runtime_base = tmp_path / "runtime"
    _seed_default_bucket(runtime_base)
    config_path = tmp_path / "config.json"
    _write_config(config_path, runtime_base, "observe")
    calls: list[int] = []
    monkeypatch.setattr(autotune.os, "fsync", lambda fd: calls.append(fd))
    autotune.run(config_path, runtime_base)
    assert calls


def test_dynamic_safety_never_exceeds_static_safety_factor(tmp_path: Path) -> None:
    runtime_base = tmp_path / "runtime"
    _seed_default_bucket(runtime_base)
    config_path = tmp_path / "config.json"
    _write_config(config_path, runtime_base, "apply")

    autotune.run(config_path, runtime_base)
    active = _read_override(runtime_base, "active_override.json")
    static_safety = json.loads(config_path.read_text(encoding="utf-8"))["gemini_filter"]["quota"]["safety_factor"]
    for model, eff_safety in active["quota_override"]["safety_factor_by_model"].items():
        assert eff_safety <= static_safety, f"{model}: effective_safety {eff_safety} exceeds static safety_factor {static_safety}"
