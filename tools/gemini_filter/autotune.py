from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
import json
import logging
import math
import os
from pathlib import Path
import time
from typing import Any, cast

from .cache_store import file_lock
from .time_utils import parse_utc_iso

LOGGER = logging.getLogger("gemini_filter.autotune")


@dataclass(frozen=True)
class ModelStats:
    reserve_ok: int
    reserve_reject_rpm: int
    reserve_reject_rpd: int
    reserve_reject_cooldown: int
    cooldown_set: int
    latency_histogram: list[int]
    latency_bucket_ms: int

    @property
    def reserve_attempts(self) -> int:
        return self.reserve_ok + self.reserve_reject_rpm + self.reserve_reject_rpd + self.reserve_reject_cooldown

    @property
    def reserve_reject_total(self) -> int:
        return self.reserve_reject_rpm + self.reserve_reject_rpd + self.reserve_reject_cooldown

    @property
    def reserve_success_rate(self) -> float:
        attempts = self.reserve_attempts
        if attempts <= 0:
            return 0.0
        return self.reserve_ok / attempts

    @property
    def quota_pressure(self) -> float:
        attempts = self.reserve_attempts
        if attempts <= 0:
            return 0.0
        return self.reserve_reject_total / attempts


def _now_utc() -> datetime:
    return datetime.now(timezone.utc)


def _to_utc_iso(dt: datetime) -> str:
    return dt.astimezone(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def _read_json(path: Path) -> dict[str, Any] | None:
    if not path.exists():
        return None
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None
    if not isinstance(raw, dict):
        return None
    return cast(dict[str, Any], raw)


def _write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".tmp")
    with temp.open("w", encoding="utf-8") as handle:
        handle.write(json.dumps(payload, ensure_ascii=True))
        handle.flush()
        os.fsync(handle.fileno())
    temp.replace(path)


def _collect_buckets(bucket_dir: Path, window_seconds: int) -> list[dict[str, Any]]:
    now_epoch = int(time.time())
    lower_bound = now_epoch - max(0, window_seconds)
    out: list[dict[str, Any]] = []
    if not bucket_dir.exists():
        return out
    for path in bucket_dir.glob("*.json"):
        data = _read_json(path)
        if data is None:
            continue
        bucket_start = str(data.get("bucket_start_utc", ""))
        bucket_epoch = parse_utc_iso(bucket_start)
        if bucket_epoch is None:
            continue
        if bucket_epoch < lower_bound:
            continue
        out.append(data)
    return out


def _aggregate_model_stats(buckets: list[dict[str, Any]]) -> dict[str, ModelStats]:
    accum: dict[str, dict[str, Any]] = {}
    for bucket in buckets:
        quota = bucket.get("quota", {})
        analyzer = bucket.get("analyzer", {})
        if isinstance(quota, dict):
            for model, model_data in quota.items():
                if not isinstance(model_data, dict):
                    continue
                entry = accum.setdefault(
                    str(model),
                    {
                        "reserve_ok": 0,
                        "reserve_reject_rpm": 0,
                        "reserve_reject_rpd": 0,
                        "reserve_reject_cooldown": 0,
                        "cooldown_set": 0,
                        "latency_histogram": [0] * 51,
                        "latency_bucket_ms": 100,
                    },
                )
                for key in ("reserve_ok", "reserve_reject_rpm", "reserve_reject_rpd", "reserve_reject_cooldown", "cooldown_set"):
                    entry[key] = int(entry[key]) + int(model_data.get(key, 0))
        if isinstance(analyzer, dict):
            for model, model_data in analyzer.items():
                if not isinstance(model_data, dict):
                    continue
                entry = accum.setdefault(
                    str(model),
                    {
                        "reserve_ok": 0,
                        "reserve_reject_rpm": 0,
                        "reserve_reject_rpd": 0,
                        "reserve_reject_cooldown": 0,
                        "cooldown_set": 0,
                        "latency_histogram": [0] * 51,
                        "latency_bucket_ms": 100,
                    },
                )
                stats = model_data.get("latency_stats", {})
                if not isinstance(stats, dict):
                    continue
                bucket_ms = int(stats.get("histogram_bucket_ms", 100))
                hist = stats.get("histogram", [])
                if not isinstance(hist, list):
                    continue
                current_hist = entry.get("latency_histogram", [])
                if not isinstance(current_hist, list):
                    current_hist = []
                merged_hist = [int(x) for x in current_hist]
                target_len = max(1, len(merged_hist), len(hist))
                if len(merged_hist) < target_len:
                    merged_hist.extend([0] * (target_len - len(merged_hist)))
                for i, count in enumerate(hist):
                    if i >= len(merged_hist):
                        break
                    merged_hist[i] = int(merged_hist[i]) + int(count)
                entry["latency_histogram"] = merged_hist
                entry["latency_bucket_ms"] = max(1, bucket_ms)
    result: dict[str, ModelStats] = {}
    for model, raw in accum.items():
        result[model] = ModelStats(
            reserve_ok=int(raw["reserve_ok"]),
            reserve_reject_rpm=int(raw["reserve_reject_rpm"]),
            reserve_reject_rpd=int(raw["reserve_reject_rpd"]),
            reserve_reject_cooldown=int(raw["reserve_reject_cooldown"]),
            cooldown_set=int(raw["cooldown_set"]),
            latency_histogram=[int(x) for x in raw["latency_histogram"]],
            latency_bucket_ms=int(raw["latency_bucket_ms"]),
        )
    return result


def _p95_latency_ms(stats: ModelStats) -> float | None:
    hist = stats.latency_histogram
    total = sum(hist)
    if total <= 0:
        return None
    target = int(math.ceil(total * 0.95))
    seen = 0
    for i, count in enumerate(hist):
        seen += count
        if seen >= target:
            return float((i + 1) * stats.latency_bucket_ms)
    return float(len(hist) * stats.latency_bucket_ms)


def _aggregate_decisions(buckets: list[dict[str, Any]]) -> dict[str, int]:
    out = {"allow": 0, "block": 0, "quota_exhausted": 0, "component_error": 0, "budget_exhausted": 0}
    for bucket in buckets:
        decisions = bucket.get("decisions", {})
        if not isinstance(decisions, dict):
            continue
        for key in out:
            out[key] += int(decisions.get(key, 0))
    return out


def _normalize_latency(p95_map: dict[str, float | None], model: str) -> float:
    p95 = p95_map.get(model)
    if p95 is None:
        return 0.5
    finite = [x for x in p95_map.values() if isinstance(x, float)]
    if not finite:
        return 0.5
    max_p95 = max(finite)
    if max_p95 <= 0:
        return 0.5
    return max(0.0, min(1.0, p95 / max_p95))


def _sort_candidates(
    candidates: list[str],
    fallback: str,
    utilities: dict[str, float],
    pressure_1h: dict[str, float],
    success_1h: dict[str, float],
    p95_1h: dict[str, float | None],
    quota_pressure_demote_threshold: float,
    success_rate_promote_threshold: float,
    latency_promote_improvement_pct: float,
) -> list[str]:
    if not candidates:
        return []
    fallback_model = fallback if fallback in candidates else candidates[-1]
    non_fallback = [m for m in candidates if m != fallback_model]
    non_fallback.sort(key=lambda m: utilities.get(m, -1e9), reverse=True)
    ordered = non_fallback + [fallback_model]
    for i, model in enumerate(list(ordered[:-1])):
        if pressure_1h.get(model, 0.0) > quota_pressure_demote_threshold:
            j = min(len(ordered) - 2, i + 1)
            if j != i:
                ordered[i], ordered[j] = ordered[j], ordered[i]
    if len(ordered) > 1:
        primary = ordered[0]
        primary_p95 = p95_1h.get(primary)
        for idx in range(1, len(ordered) - 1):
            model = ordered[idx]
            model_p95 = p95_1h.get(model)
            if success_1h.get(model, 0.0) <= success_rate_promote_threshold:
                continue
            if primary_p95 is None or model_p95 is None or primary_p95 <= 0:
                continue
            improvement_pct = (primary_p95 - model_p95) * 100.0 / primary_p95
            if improvement_pct > latency_promote_improvement_pct:
                ordered[idx - 1], ordered[idx] = ordered[idx], ordered[idx - 1]
                break
    return ordered


def _cheapest_first(candidates: list[str], fallback: str, quota_models: dict[str, Any]) -> list[str]:
    if not candidates:
        return []
    fallback_model = fallback if fallback in candidates else candidates[-1]
    pool = [m for m in candidates if m != fallback_model]
    pool.sort(key=lambda m: int(cast(dict[str, Any], quota_models.get(m, {})).get("rpm", 10**9)))
    return pool + [fallback_model]


def _cleanup_buckets(bucket_dir: Path, retention_seconds: int) -> None:
    if not bucket_dir.exists():
        return
    now_epoch = int(time.time())
    current_minute = now_epoch - (now_epoch % 60)
    for path in bucket_dir.glob("*.json"):
        bucket_epoch: int | None = None
        data = _read_json(path)
        if data is not None:
            bucket_epoch = parse_utc_iso(str(data.get("bucket_start_utc", "")))
        if bucket_epoch is None:
            try:
                bucket_epoch = int(path.stat().st_mtime)
            except Exception:
                continue
        if bucket_epoch >= current_minute:
            continue
        if now_epoch - bucket_epoch <= retention_seconds:
            continue
        try:
            path.unlink()
        except Exception:
            continue
    for path in bucket_dir.glob("*.tmp"):
        try:
            mtime = int(path.stat().st_mtime)
        except Exception:
            continue
        if now_epoch - mtime <= min(retention_seconds, 3600):
            continue
        try:
            path.unlink()
        except Exception:
            continue


def _build_override(gemini_cfg: dict[str, Any], autotune_cfg: dict[str, Any], runtime_base: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    model_routing = gemini_cfg.get("model_routing", {})
    model_routing = model_routing if isinstance(model_routing, dict) else {}
    quota_cfg = gemini_cfg.get("quota", {})
    quota_cfg = quota_cfg if isinstance(quota_cfg, dict) else {}
    quota_models = quota_cfg.get("models", {})
    quota_models = quota_models if isinstance(quota_models, dict) else {}

    weights = autotune_cfg.get("weights", {})
    weights = weights if isinstance(weights, dict) else {}
    w1 = float(weights.get("reserve_success_rate", 0.45))
    w2 = float(weights.get("quota_pressure", 0.30))
    w3 = float(weights.get("normalized_latency", 0.15))
    w4 = float(weights.get("decision_quality_proxy", 0.10))

    min_samples = max(1, int(autotune_cfg.get("min_samples_per_model", 10)))
    demote_threshold = float(autotune_cfg.get("quota_pressure_demote_threshold", 0.40))
    promote_success = float(autotune_cfg.get("success_rate_promote_threshold", 0.95))
    promote_latency_pct = float(autotune_cfg.get("latency_promote_improvement_pct", 20.0))
    emergency_limit = max(1, int(autotune_cfg.get("emergency_quota_exhausted_cycles", 3)))
    ttl_seconds = max(60, int(autotune_cfg.get("override_ttl_seconds", 1800)))
    max_block_rate_increase_pct = float(autotune_cfg.get("max_block_rate_increase_pct", 10.0))
    block_rate_window_seconds = max(60, int(autotune_cfg.get("block_rate_window_seconds", 300)))
    retention_seconds = max(3600, int(autotune_cfg.get("bucket_retention_seconds", 90000)))

    buckets_dir = runtime_base / "cache" / "metrics" / "buckets"
    buckets_1h = _collect_buckets(buckets_dir, 3600)
    buckets_24h = _collect_buckets(buckets_dir, 24 * 3600)
    buckets_5m = _collect_buckets(buckets_dir, block_rate_window_seconds)

    stats_1h = _aggregate_model_stats(buckets_1h)
    stats_24h = _aggregate_model_stats(buckets_24h)
    p95_1h = {model: _p95_latency_ms(stat) for model, stat in stats_1h.items()}

    all_models = sorted(set(stats_1h.keys()) | set(stats_24h.keys()) | set(quota_models.keys()))
    utilities: dict[str, float] = {}
    pressure_1h: dict[str, float] = {}
    success_1h: dict[str, float] = {}
    for model in all_models:
        stat = stats_1h.get(
            model,
            ModelStats(0, 0, 0, 0, 0, [0] * 51, 100),
        )
        pressure_1h[model] = stat.quota_pressure
        success_1h[model] = stat.reserve_success_rate
        normalized_latency = _normalize_latency(p95_1h, model)
        quality_proxy = 0.5
        if stat.reserve_attempts < min_samples:
            utilities[model] = 0.5
        else:
            utilities[model] = (
                w1 * stat.reserve_success_rate
                - w2 * stat.quota_pressure
                - w3 * normalized_latency
                + w4 * quality_proxy
            )

    sentiment_cfg = model_routing.get("sentiment", {})
    sentiment_cfg = sentiment_cfg if isinstance(sentiment_cfg, dict) else {}
    vision_cfg = model_routing.get("vision", {})
    vision_cfg = vision_cfg if isinstance(vision_cfg, dict) else {}
    sentiment_candidates = [str(x).strip() for x in sentiment_cfg.get("candidates", []) if str(x).strip()]
    vision_candidates = [str(x).strip() for x in vision_cfg.get("candidates", []) if str(x).strip()]
    if not sentiment_candidates:
        sentiment_candidates = [str(gemini_cfg.get("sentiment_model", ""))]
    if not vision_candidates:
        vision_candidates = [str(gemini_cfg.get("vision_model", ""))]
    sentiment_fallback = sentiment_candidates[-1]
    vision_fallback = vision_candidates[-1]

    sorted_sentiment = _sort_candidates(
        sentiment_candidates,
        sentiment_fallback,
        utilities,
        pressure_1h,
        success_1h,
        p95_1h,
        demote_threshold,
        promote_success,
        promote_latency_pct,
    )
    sorted_vision = _sort_candidates(
        vision_candidates,
        vision_fallback,
        utilities,
        pressure_1h,
        success_1h,
        p95_1h,
        demote_threshold,
        promote_success,
        promote_latency_pct,
    )

    decisions_5m = _aggregate_decisions(buckets_5m)
    decisions_24h = _aggregate_decisions(buckets_24h)
    decisions_now = decisions_5m["allow"] + decisions_5m["block"]
    block_rate_now = (decisions_5m["block"] / decisions_now) if decisions_now > 0 else 0.0
    block_den_24h = decisions_24h["allow"] + decisions_24h["block"]
    block_rate_24h = (decisions_24h["block"] / block_den_24h) if block_den_24h > 0 else 0.0

    state_path = runtime_base / "cache" / "autotune" / "state.json"
    state_raw = _read_json(state_path) or {}
    state = dict(state_raw)
    consecutive = int(state.get("consecutive_quota_exhausted_cycles", 0))
    previous_quota_exhausted_5m = max(0, int(state.get("last_quota_exhausted_5m", 0)))
    current_quota_exhausted_5m = max(0, int(decisions_5m.get("quota_exhausted", 0)))
    if current_quota_exhausted_5m > previous_quota_exhausted_5m:
        consecutive += 1
    else:
        consecutive = 0
    state["schema_version"] = 1
    state["consecutive_quota_exhausted_cycles"] = consecutive
    state["last_quota_exhausted_5m"] = current_quota_exhausted_5m
    state["last_updated"] = _to_utc_iso(_now_utc())

    override_type = "normal"
    reason_summary: list[str] = []
    reason_summary.append(f"block_rate_now={block_rate_now:.4f} block_rate_24h={block_rate_24h:.4f}")

    if consecutive >= emergency_limit:
        override_type = "emergency"
        reason_summary.append(f"emergency quota_exhausted consecutive={consecutive}")
        sorted_sentiment = _cheapest_first(sorted_sentiment, sentiment_fallback, quota_models)
        sorted_vision = _cheapest_first(sorted_vision, vision_fallback, quota_models)

    quota_override_models: dict[str, dict[str, int]] = {}
    safety_by_model: dict[str, float] = {}
    base_safety = max(0.05, min(1.0, float(quota_cfg.get("safety_factor", 0.7))))
    for model_name, model_limit in quota_models.items():
        if not isinstance(model_limit, dict):
            continue
        try:
            static_rpm = int(model_limit.get("rpm", 0))
            static_rpd = int(model_limit.get("rpd", 0))
        except Exception:
            continue
        if static_rpm <= 0 or static_rpd <= 0:
            continue
        pressure = pressure_1h.get(str(model_name), 0.0)
        eff_safety = max(0.35, min(base_safety, base_safety * (1.0 - pressure)))
        if override_type == "emergency":
            eff_safety = min(base_safety, 0.5)
        dyn_rpm = max(1, min(static_rpm, int(math.floor(static_rpm * eff_safety))))
        dyn_rpd = max(1, min(static_rpd, int(math.floor(static_rpd * eff_safety))))
        quota_override_models[str(model_name)] = {"rpm": dyn_rpm, "rpd": dyn_rpd}
        safety_by_model[str(model_name)] = eff_safety

    active_override_path = runtime_base / "cache" / "autotune" / "active_override.json"
    current_override = _read_json(active_override_path) or {}
    baseline = float(state.get("active_override_baseline", {}).get("block_rate_24h_before_apply", block_rate_24h))
    rollback_triggered = (
        current_override and
        decisions_now > 0 and
        block_rate_now > baseline * (1.0 + max_block_rate_increase_pct / 100.0)
    )
    if override_type != "emergency" and rollback_triggered:
        override_type = "rollback"
        reason_summary.append(
            f"rollback block_rate_now={block_rate_now:.4f} baseline={baseline:.4f} threshold_pct={max_block_rate_increase_pct:.2f}"
        )
        sorted_sentiment = sentiment_candidates
        sorted_vision = vision_candidates
        quota_override_models = {
            str(k): {"rpm": int(v.get("rpm", 0)), "rpd": int(v.get("rpd", 0))}
            for k, v in quota_models.items()
            if isinstance(v, dict) and int(v.get("rpm", 0)) > 0 and int(v.get("rpd", 0)) > 0
        }
        safety_by_model = {str(k): base_safety for k in quota_override_models.keys()}
    elif override_type == "emergency" and rollback_triggered:
        reason_summary.append("rollback signal ignored because emergency override has higher priority")

    now = _now_utc()
    override = {
        "schema_version": 1,
        "generated_at": _to_utc_iso(now),
        "expiry_at": _to_utc_iso(now + timedelta(seconds=ttl_seconds)),
        "override_type": override_type,
        "model_routing_override": {
            "sentiment": {"candidates": sorted_sentiment},
            "vision": {
                "candidates": sorted_vision,
                "pro_escalation_enabled": bool(vision_cfg.get("pro_escalation_enabled", False)) and override_type != "emergency",
                "pro_escalation_min_score": float(vision_cfg.get("pro_escalation_min_score", 0.45)),
                "pro_escalation_max_score": float(vision_cfg.get("pro_escalation_max_score", 0.65)),
            },
        },
        "quota_override": {
            "safety_factor_by_model": safety_by_model,
            "models": quota_override_models,
        },
        "reason_summary": reason_summary,
        "validation": {
            "decisions_5m": decisions_5m,
            "decisions_24h": decisions_24h,
            "retention_seconds": retention_seconds,
        },
    }
    state["active_override_generated_at"] = override["generated_at"]
    state["active_override_baseline"] = {"block_rate_24h_before_apply": block_rate_24h}
    return override, state


def run(config_path: Path, runtime_base_dir: Path | None) -> int:
    root_config = _read_json(config_path)
    if root_config is None:
        LOGGER.warning("autotune config missing path=%s", config_path)
        return 0
    gemini_cfg = root_config.get("gemini_filter", {})
    if not isinstance(gemini_cfg, dict):
        return 0
    autotune_cfg = gemini_cfg.get("autotune", {})
    if not isinstance(autotune_cfg, dict):
        return 0
    if not bool(autotune_cfg.get("enabled", False)):
        return 0
    mode = str(autotune_cfg.get("mode", "observe")).strip().lower()
    if mode == "disabled":
        return 0

    runtime_base = runtime_base_dir
    if runtime_base is None:
        runtime_base = Path(str(gemini_cfg.get("runtime_dir", "tmp/gemini_filter")))
    runtime_base = runtime_base.resolve()
    autotune_dir = runtime_base / "cache" / "autotune"
    autotune_dir.mkdir(parents=True, exist_ok=True)
    lock_path = autotune_dir / "controller.lock"
    state_path = autotune_dir / "state.json"
    last_recommendation_path = autotune_dir / "last_recommendation.json"
    active_override_path = autotune_dir / "active_override.json"
    retention_seconds = max(3600, int(autotune_cfg.get("bucket_retention_seconds", 90000)))

    try:
        with file_lock(lock_path, timeout_seconds=0.1, stale_seconds=5.0):
            override, state = _build_override(gemini_cfg, autotune_cfg, runtime_base)
            _write_json_atomic(last_recommendation_path, override)
            routing = override.get("model_routing_override", {})
            routing = routing if isinstance(routing, dict) else {}
            sentiment = routing.get("sentiment", {})
            sentiment = sentiment if isinstance(sentiment, dict) else {}
            vision = routing.get("vision", {})
            vision = vision if isinstance(vision, dict) else {}
            sentiment_candidates = sentiment.get("candidates", [])
            if not isinstance(sentiment_candidates, list):
                sentiment_candidates = []
            vision_candidates = vision.get("candidates", [])
            if not isinstance(vision_candidates, list):
                vision_candidates = []
            LOGGER.info(
                "autotune recommendation mode=%s type=%s sentiment_candidates=%s vision_candidates=%s",
                mode,
                override.get("override_type", "unknown"),
                ",".join(str(x) for x in sentiment_candidates),
                ",".join(str(x) for x in vision_candidates),
            )
            if mode == "apply":
                _write_json_atomic(active_override_path, override)
            _write_json_atomic(state_path, state)
            _cleanup_buckets(runtime_base / "cache" / "metrics" / "buckets", retention_seconds)
    except TimeoutError:
        LOGGER.info("autotune skipped reason=lock_busy")
        return 0
    except Exception as exc:  # noqa: BLE001
        LOGGER.exception("autotune failed err=%s", exc)
        _cleanup_buckets(runtime_base / "cache" / "metrics" / "buckets", retention_seconds)
        return 0
    return 0


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(levelname).1s %(name)s | %(message)s")
    parser = argparse.ArgumentParser(description="Gemini autotune controller")
    parser.add_argument("--config", default="config.json")
    parser.add_argument("--runtime-base-dir", default=None)
    args = parser.parse_args()
    runtime_base = Path(args.runtime_base_dir) if args.runtime_base_dir else None
    return run(Path(args.config), runtime_base)


if __name__ == "__main__":
    raise SystemExit(main())
