from __future__ import annotations

import json
import math
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo

from .cache_store import file_lock

PACIFIC_TZ = ZoneInfo("America/Los_Angeles")


@dataclass(frozen=True)
class QuotaConfig:
    enabled: bool
    safety_factor: float
    cooldown_seconds_on_429: int
    default_rpm: int
    default_rpd: int
    model_limits: dict[str, tuple[int, int]]


@dataclass(frozen=True)
class ReserveResult:
    ok: bool
    retry_after_seconds: int
    reason: str


class QuotaManager:
    def __init__(self, root: Path, config: QuotaConfig, metrics_store: Any = None) -> None:
        self._root = root
        self._config = config
        self._state_path = root / "cache" / "quota" / "state.json"
        self._lock_path = root / "cache" / "quota" / "state.lock"
        self._metrics_store = metrics_store
        self._state_path.parent.mkdir(parents=True, exist_ok=True)

    def reserve(self, model: str, cost: int = 1) -> ReserveResult:
        if not self._config.enabled:
            self._metric(model, "reserve_bypass")
            return ReserveResult(ok=True, retry_after_seconds=0, reason="quota disabled")
        if cost <= 0:
            self._metric(model, "reserve_zero_cost")
            return ReserveResult(ok=True, retry_after_seconds=0, reason="no cost")

        with file_lock(self._lock_path):
            state = self._read_state()
            now = time.time()
            model_state = state.setdefault("models", {}).setdefault(model, {})

            cooldown_until = float(model_state.get("cooldown_until", 0.0))
            if cooldown_until > now:
                retry_after = int(math.ceil(cooldown_until - now))
                self._metric(model, "reserve_reject_cooldown")
                return ReserveResult(ok=False, retry_after_seconds=max(1, retry_after), reason="model cooldown")

            rpm_limit = max(1, int(self._config.default_rpm * self._config.safety_factor))
            rpd_limit = max(1, int(self._config.default_rpd * self._config.safety_factor))
            if model in self._config.model_limits:
                model_rpm, model_rpd = self._config.model_limits[model]
                rpm_limit = max(1, int(model_rpm * self._config.safety_factor))
                rpd_limit = max(1, int(model_rpd * self._config.safety_factor))
            capacity = float(rpm_limit)
            refill_per_second = capacity / 60.0

            tokens = float(model_state.get("tokens", capacity))
            last_refill = float(model_state.get("last_refill", now))
            elapsed = max(0.0, now - last_refill)
            tokens = min(capacity, tokens + elapsed * refill_per_second)

            day_key = datetime.now(PACIFIC_TZ).strftime("%Y-%m-%d")
            used_day_key = str(model_state.get("rpd_day", day_key))
            used_today = int(model_state.get("rpd_used", 0))
            if used_day_key != day_key:
                used_today = 0
                used_day_key = day_key

            if used_today + cost > rpd_limit:
                retry = self._seconds_until_next_pacific_day()
                self._metric(model, "reserve_reject_rpd")
                return ReserveResult(ok=False, retry_after_seconds=max(1, retry), reason="rpd exhausted")
            if tokens < cost:
                missing = float(cost) - tokens
                retry = int(math.ceil(missing / refill_per_second))
                self._metric(model, "reserve_reject_rpm")
                return ReserveResult(ok=False, retry_after_seconds=max(1, retry), reason="rpm exhausted")

            tokens -= float(cost)
            used_today += cost
            model_state["tokens"] = tokens
            model_state["last_refill"] = now
            model_state["rpd_day"] = used_day_key
            model_state["rpd_used"] = used_today
            self._write_state(state)
            self._metric(model, "reserve_ok")
            return ReserveResult(ok=True, retry_after_seconds=0, reason="reserved")

    def cooldown(self, model: str) -> None:
        if not self._config.enabled:
            return
        with file_lock(self._lock_path):
            state = self._read_state()
            model_state = state.setdefault("models", {}).setdefault(model, {})
            model_state["cooldown_until"] = time.time() + self._config.cooldown_seconds_on_429
            self._write_state(state)
            self._metric(model, "cooldown_set")

    def _read_state(self) -> dict[str, Any]:
        if not self._state_path.exists():
            return {"models": {}}
        try:
            raw = json.loads(self._state_path.read_text(encoding="utf-8"))
            if isinstance(raw, dict):
                return raw
        except Exception:
            pass
        return {"models": {}}

    def _write_state(self, state: dict[str, Any]) -> None:
        temp = self._state_path.with_suffix(".tmp")
        temp.write_text(json.dumps(state, ensure_ascii=True), encoding="utf-8")
        temp.replace(self._state_path)

    def _metric(self, model: str, metric: str) -> None:
        if self._metrics_store is None:
            return
        try:
            self._metrics_store.incr_quota(model, metric)
        except Exception:
            pass

    @staticmethod
    def _seconds_until_next_pacific_day() -> int:
        now = datetime.now(PACIFIC_TZ)
        tomorrow = now.replace(hour=0, minute=0, second=0, microsecond=0).timestamp() + 24 * 3600
        return int(max(1, math.ceil(tomorrow - now.timestamp())))
