from __future__ import annotations

import hashlib
import json
import os
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator, cast

_LOCK_TIMEOUT_SECONDS = 2.0
_LOCK_STALE_SECONDS = 30.0


def _ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def _key_path(root: Path, namespace: str, key: str) -> Path:
    digest = hashlib.sha256(key.encode("utf-8")).hexdigest()
    return root / namespace / f"{digest}.json"


@contextmanager
def file_lock(
    lock_path: Path,
    timeout_seconds: float = _LOCK_TIMEOUT_SECONDS,
    stale_seconds: float = _LOCK_STALE_SECONDS,
) -> Iterator[None]:
    _ensure_dir(lock_path.parent)
    deadline = time.monotonic() + timeout_seconds
    while True:
        try:
            fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.close(fd)
            break
        except (FileExistsError, PermissionError):
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


def load_entry(root: Path, namespace: str, key: str) -> dict[str, Any] | None:
    path = _key_path(root, namespace, key)
    if not path.exists():
        return None
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(raw, dict):
            return None
        return cast(dict[str, Any], raw)
    except Exception:
        return None


def save_entry(root: Path, namespace: str, key: str, payload: dict[str, Any]) -> None:
    path = _key_path(root, namespace, key)
    _ensure_dir(path.parent)
    lock_path = path.with_suffix(path.suffix + ".lock")
    with file_lock(lock_path):
        temp_path: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                delete=False,
                dir=path.parent,
                prefix=path.name + ".",
                suffix=".tmp",
            ) as tmp:
                tmp.write(json.dumps(payload, ensure_ascii=True))
                temp_path = Path(tmp.name)
            if temp_path is None:
                raise RuntimeError("temporary cache file was not created")
            temp_path.replace(path)
        finally:
            if temp_path is not None and temp_path.exists():
                try:
                    temp_path.unlink()
                except OSError:
                    pass
