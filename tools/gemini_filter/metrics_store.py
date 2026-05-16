from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .cache_store import file_lock


class MetricsStore:
    def __init__(self, root: Path) -> None:
        self._state_path = root / "cache" / "metrics" / "state.json"
        self._lock_path = root / "cache" / "metrics" / "state.lock"
        self._state_path.parent.mkdir(parents=True, exist_ok=True)

    def incr_cache(self, component: str, metric: str, delta: int = 1) -> None:
        self._incr(("cache", component, metric), delta)

    def incr_quota(self, model: str, metric: str, delta: int = 1) -> None:
        self._incr(("quota", model, metric), delta)

    def snapshot(self) -> dict[str, Any]:
        with file_lock(self._lock_path):
            return self._read_state()

    def _incr(self, path: tuple[str, str, str], delta: int) -> None:
        with file_lock(self._lock_path):
            state = self._read_state()
            level = state
            for key in path[:-1]:
                level = level.setdefault(key, {})
            leaf = path[-1]
            level[leaf] = int(level.get(leaf, 0)) + delta
            self._write_state(state)

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

