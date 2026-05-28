from __future__ import annotations

from datetime import datetime, timezone
import json
from pathlib import Path
from typing import Any

from .cache_store import file_lock

_LATENCY_BUCKET_MS = 100
_LATENCY_BUCKET_COUNT = 51  # 0..49 => 0-4999ms, 50 => overflow


class MetricsStore:
    def __init__(self, root: Path) -> None:
        self._root = root
        self._state_path = root / "cache" / "metrics" / "state.json"
        self._lock_path = root / "cache" / "metrics" / "state.lock"
        self._bucket_dir = root / "cache" / "metrics" / "buckets"
        self._state_path.parent.mkdir(parents=True, exist_ok=True)
        self._bucket_dir.mkdir(parents=True, exist_ok=True)

    def incr_cache(self, component: str, metric: str, delta: int = 1) -> None:
        self._incr_state_and_bucket(("cache", component, metric), delta)

    def incr_quota(self, model: str, metric: str, delta: int = 1) -> None:
        self._incr_state_and_bucket(("quota", model, metric), delta)

    def record_analyzer_latency(self, model: str, component: str, latency_ms: int) -> None:
        clamped_ms = max(0, int(latency_ms))
        with file_lock(self._lock_path):
            bucket_path, bucket = self._load_bucket_locked()
            analyzer = bucket.setdefault("analyzer", {})
            model_data = analyzer.setdefault(model, {})
            model_data[f"{component}_calls"] = int(model_data.get(f"{component}_calls", 0)) + 1
            stats = model_data.setdefault(
                "latency_stats",
                {
                    "count": 0,
                    "sum_ms": 0,
                    "min_ms": 0,
                    "max_ms": 0,
                    "histogram_bucket_ms": _LATENCY_BUCKET_MS,
                    "histogram": [0] * _LATENCY_BUCKET_COUNT,
                },
            )
            count = int(stats.get("count", 0))
            sum_ms = int(stats.get("sum_ms", 0))
            if count <= 0:
                min_ms = clamped_ms
                max_ms = clamped_ms
            else:
                min_ms = min(int(stats.get("min_ms", clamped_ms)), clamped_ms)
                max_ms = max(int(stats.get("max_ms", clamped_ms)), clamped_ms)
            histogram = stats.get("histogram", [])
            if not isinstance(histogram, list) or len(histogram) != _LATENCY_BUCKET_COUNT:
                histogram = [0] * _LATENCY_BUCKET_COUNT
            bin_index = min(_LATENCY_BUCKET_COUNT - 1, clamped_ms // _LATENCY_BUCKET_MS)
            histogram[bin_index] = int(histogram[bin_index]) + 1
            stats["count"] = count + 1
            stats["sum_ms"] = sum_ms + clamped_ms
            stats["min_ms"] = min_ms
            stats["max_ms"] = max_ms
            stats["histogram_bucket_ms"] = _LATENCY_BUCKET_MS
            stats["histogram"] = histogram
            self._write_bucket_locked(bucket_path, bucket)

    def record_decision(self, decision: str, error_code: str | None) -> None:
        with file_lock(self._lock_path):
            bucket_path, bucket = self._load_bucket_locked()
            decisions = bucket.setdefault("decisions", {})
            normalized = decision.strip().lower()
            if normalized == "allow":
                key = "allow"
            else:
                key = "block"
            decisions[key] = int(decisions.get(key, 0)) + 1
            if error_code:
                err_key = str(error_code).strip().lower()
                if err_key in ("quota_exhausted", "component_error", "budget_exhausted"):
                    decisions[err_key] = int(decisions.get(err_key, 0)) + 1
            self._write_bucket_locked(bucket_path, bucket)

    def snapshot(self) -> dict[str, Any]:
        with file_lock(self._lock_path):
            return self._read_state()

    def _incr_state_and_bucket(self, path: tuple[str, str, str], delta: int) -> None:
        with file_lock(self._lock_path):
            state = self._read_state()
            state_level = state
            for key in path[:-1]:
                state_level = state_level.setdefault(key, {})
            state_leaf = path[-1]
            state_level[state_leaf] = int(state_level.get(state_leaf, 0)) + delta
            self._write_state(state)

            bucket_path, bucket = self._load_bucket_locked()
            level = bucket
            for key in path[:-1]:
                level = level.setdefault(key, {})
            leaf = path[-1]
            level[leaf] = int(level.get(leaf, 0)) + delta
            self._write_bucket_locked(bucket_path, bucket)

    def _bucket_path_for_now(self) -> tuple[Path, str]:
        now = datetime.now(timezone.utc)
        bucket_start = now.replace(second=0, microsecond=0)
        bucket_id = bucket_start.strftime("%Y-%m-%dT%H-%M")
        return self._bucket_dir / f"{bucket_id}.json", bucket_start.strftime("%Y-%m-%dT%H:%M:%SZ")

    def _load_bucket_locked(self) -> tuple[Path, dict[str, Any]]:
        path, bucket_start = self._bucket_path_for_now()
        if path.exists():
            try:
                raw = json.loads(path.read_text(encoding="utf-8"))
                if isinstance(raw, dict):
                    return path, raw
            except Exception:
                pass
        return path, {
            "schema_version": 1,
            "bucket_start_utc": bucket_start,
            "cache": {},
            "quota": {},
            "analyzer": {},
            "decisions": {"allow": 0, "block": 0, "quota_exhausted": 0, "component_error": 0},
        }

    def _write_bucket_locked(self, path: Path, payload: dict[str, Any]) -> None:
        temp = path.with_suffix(".tmp")
        temp.write_text(json.dumps(payload, ensure_ascii=True), encoding="utf-8")
        temp.replace(path)

    def _read_state(self) -> dict[str, Any]:
        if not self._state_path.exists():
            return {"cache": {}, "quota": {}}
        try:
            raw = json.loads(self._state_path.read_text(encoding="utf-8"))
            if isinstance(raw, dict):
                raw.setdefault("cache", {})
                raw.setdefault("quota", {})
                return raw
        except Exception:
            pass
        return {"cache": {}, "quota": {}}

    def _write_state(self, state: dict[str, Any]) -> None:
        temp = self._state_path.with_suffix(".tmp")
        temp.write_text(json.dumps(state, ensure_ascii=True), encoding="utf-8")
        temp.replace(self._state_path)
