from __future__ import annotations

import hashlib
import json
import os
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator, cast


def _ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def _key_path(root: Path, namespace: str, key: str) -> Path:
    digest = hashlib.sha256(key.encode("utf-8")).hexdigest()
    return root / namespace / f"{digest}.json"


@contextmanager
def file_lock(lock_path: Path, timeout_seconds: float = 2.0) -> Iterator[None]:
    _ensure_dir(lock_path.parent)
    deadline = time.time() + timeout_seconds
    while True:
        try:
            fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.close(fd)
            break
        except FileExistsError:
            if time.time() >= deadline:
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
    temp = path.with_suffix(".tmp")
    temp.write_text(json.dumps(payload, ensure_ascii=True), encoding="utf-8")
    temp.replace(path)
