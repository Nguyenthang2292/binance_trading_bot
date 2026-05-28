from __future__ import annotations

import hashlib
import json
import logging
import os
import random
import re
import tempfile
import time
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterator, TypeVar

from google import genai
from google.genai import errors, types as genai_types

T = TypeVar("T")

_HTTP_TIMEOUT_MS = 30_000  # default; override via constructor
_STATE_DIR_ENV = "GEMINI_KEY_MANAGER_STATE_DIR"
_LOCK_TIMEOUT_SECONDS = 2.0
_LOCK_STALE_SECONDS = 30.0
_LOG = logging.getLogger(__name__)
_KEY_ERROR_PATTERNS: tuple[re.Pattern[str], ...] = (
    re.compile(r"\b429\b"),
    re.compile(r"\bresource[_\s-]?exhausted\b"),
    re.compile(r"\btoo\s+many\s+requests\b"),
    re.compile(r"\bquota(?:\s+exceeded)?\b"),
    re.compile(r"\brate[\s_-]?limit(?:ed|ing)?\b"),
    re.compile(r"\bapi[-\s]?key\b"),
    re.compile(r"\bunauthori[sz]ed\b"),
)


@dataclass(frozen=True)
class GeminiKey:
    name: str
    value: str


def _is_retryable_key_error(exc: Exception) -> bool:
    if isinstance(exc, errors.APIError):
        code = getattr(exc, "code", None) or getattr(exc, "status_code", None)
        # Rotate keys only for key-scoped failures.
        # 5xx/server outages are shared failure domains and should fail fast so
        # callers can handle global backoff/sleep once instead of N-key fanout.
        if code is not None:
            return code in {401, 429}
    lowered = str(exc).lower()
    return any(pattern.search(lowered) for pattern in _KEY_ERROR_PATTERNS)


class GeminiKeyManager:
    def __init__(self, http_timeout_ms: int = _HTTP_TIMEOUT_MS, state_dir: str | Path | None = None) -> None:
        self._http_timeout_ms = http_timeout_ms
        keys = _collect_keys()

        if not keys:
            raise RuntimeError("No Gemini API key found in environment")

        self._keys = keys
        self._keyset_digest = _keyset_digest(keys)
        self._state_dir = _resolve_state_dir(state_dir)

    @property
    def key_count(self) -> int:
        return len(self._keys)

    @property
    def key_names(self) -> tuple[str, ...]:
        return tuple(key.name for key in self._keys)

    def run_with_rotation(self, fn: Callable[[genai.Client, GeminiKey], T]) -> T:
        last_error: Exception | None = None
        start = self._claim_start_index()
        for offset in range(len(self._keys)):
            key = self._keys[(start + offset) % len(self._keys)]
            client = genai.Client(
                api_key=key.value,
                http_options=genai_types.HttpOptions(timeout=self._http_timeout_ms),
            )
            try:
                return fn(client, key)
            except Exception as exc:
                if not _is_retryable_key_error(exc):
                    raise
                last_error = exc
            finally:
                try:
                    client.close()
                except Exception as close_exc:
                    _LOG.warning(
                        "gemini_key_manager: failed to close genai.Client for key %s: %s",
                        key.name,
                        close_exc,
                    )
        raise RuntimeError(f"All {len(self._keys)} Gemini keys failed: {last_error}") from last_error

    def _claim_start_index(self) -> int:
        if len(self._keys) == 1:
            return 0
        state_path = self._state_dir / "state.json"
        lock_path = self._state_dir / "state.lock"
        try:
            with _file_lock(lock_path):
                state = _read_state(state_path)
                if state.get("keyset_digest") != self._keyset_digest:
                    state = {"keyset_digest": self._keyset_digest, "next_index": 0}
                next_index = int(state.get("next_index", 0)) % len(self._keys)
                state["next_index"] = (next_index + 1) % len(self._keys)
                state["key_count"] = len(self._keys)
                state["updated_at"] = time.time()
                _write_state(state_path, state)
                return next_index
        except (TimeoutError, OSError, ValueError, TypeError, json.JSONDecodeError) as exc:
            fallback = random.randrange(len(self._keys))
            _LOG.warning(
                "gemini_key_manager: failed to claim round-robin start index, fallback to random %d: %s",
                fallback,
                exc,
            )
            return fallback


def _split_key_list(raw: str) -> list[str]:
    normalized = raw.replace(";", ",").replace("\n", ",")
    return [part.strip() for part in normalized.split(",") if part.strip()]


def _collect_keys() -> list[GeminiKey]:
    keys: list[GeminiKey] = []
    seen_values: set[str] = set()

    def add(name: str, value: str | None) -> None:
        if value is None:
            return
        normalized = value.strip()
        if not normalized or normalized in seen_values:
            return
        seen_values.add(normalized)
        keys.append(GeminiKey(name=name, value=normalized))

    index = 1
    while True:
        value = os.getenv(f"GEMINI_API_KEY_{index}")
        if not value:
            break
        add(f"GEMINI_API_KEY_{index}", value)
        index += 1

    add("GEMINI_API_KEY", os.getenv("GEMINI_API_KEY"))

    packed_keys = os.getenv("GEMINI_API_KEYS")
    if packed_keys:
        for packed_index, value in enumerate(_split_key_list(packed_keys), start=1):
            add(f"GEMINI_API_KEYS_{packed_index}", value)

    add("GEMINI_TEXT_API_KEY", os.getenv("GEMINI_TEXT_API_KEY"))
    return keys


def _keyset_digest(keys: list[GeminiKey]) -> str:
    # Hash values only so env-var renames do not reset rotation state.
    material = "\n".join(key.value for key in keys)
    return hashlib.sha256(material.encode("utf-8")).hexdigest()


def _resolve_state_dir(state_dir: str | Path | None) -> Path:
    if state_dir is not None:
        return Path(state_dir).expanduser().resolve(strict=False)
    configured = os.getenv(_STATE_DIR_ENV)
    if configured:
        return Path(configured).expanduser().resolve(strict=False)
    return (Path(tempfile.gettempdir()) / "gemini_key_manager").resolve(strict=False)


@contextmanager
def _file_lock(
    lock_path: Path,
    timeout_seconds: float = _LOCK_TIMEOUT_SECONDS,
    stale_seconds: float = _LOCK_STALE_SECONDS,
) -> Iterator[None]:
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + timeout_seconds
    while True:
        try:
            fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.close(fd)
            break
        except FileExistsError:
            if stale_seconds > 0:
                try:
                    age_seconds = time.time() - lock_path.stat().st_mtime
                    if age_seconds >= stale_seconds:
                        lock_path.unlink(missing_ok=True)
                        continue
                except FileNotFoundError:
                    continue
                except OSError:
                    pass
            if time.monotonic() >= deadline:
                raise TimeoutError(f"lock timeout: {lock_path}")
            time.sleep(0.02)
    try:
        yield
    finally:
        try:
            lock_path.unlink()
        except FileNotFoundError:
            pass


def _read_state(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(raw, dict):
            return raw
    except Exception:
        pass
    return {}


def _write_state(path: Path, state: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(".tmp")
    payload = json.dumps(state, ensure_ascii=True)
    # Advisory state: atomic replace is used; fsync improves durability of file
    # contents but we intentionally skip directory fsync for simplicity.
    with temp.open("w", encoding="utf-8") as handle:
        handle.write(payload)
        handle.flush()
        os.fsync(handle.fileno())
    temp.replace(path)
