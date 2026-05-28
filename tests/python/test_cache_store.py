from __future__ import annotations

import os
from pathlib import Path
import threading
import time

import pytest

from tools.gemini_filter.cache_store import file_lock, load_entry, save_entry


def test_save_and_load_entry(tmp_path: Path) -> None:
    root = tmp_path / "root"
    save_entry(root, "ns", "k1", {"value": 123})
    loaded = load_entry(root, "ns", "k1")
    assert loaded == {"value": 123}


def test_load_missing_entry_returns_none(tmp_path: Path) -> None:
    loaded = load_entry(tmp_path / "root", "ns", "missing")
    assert loaded is None


def test_file_lock_recovers_stale_lock(tmp_path: Path) -> None:
    lock_path = tmp_path / "root" / "state.lock"
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock_path.write_text("locked", encoding="utf-8")
    stale_mtime = time.time() - 120
    os.utime(lock_path, (stale_mtime, stale_mtime))

    with file_lock(lock_path, timeout_seconds=0.0, stale_seconds=1.0):
        assert lock_path.exists()

    assert not lock_path.exists()


def test_file_lock_times_out_for_fresh_lock(tmp_path: Path) -> None:
    lock_path = tmp_path / "root" / "state.lock"
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock_path.write_text("locked", encoding="utf-8")

    with pytest.raises(TimeoutError):
        with file_lock(lock_path, timeout_seconds=0.0, stale_seconds=60.0):
            pass


def test_save_entry_concurrent_writers_same_key(tmp_path: Path) -> None:
    root = tmp_path / "root"
    errors: list[Exception] = []

    def writer(value: int) -> None:
        try:
            save_entry(root, "ns", "same-key", {"value": value})
        except Exception as exc:  # noqa: BLE001
            errors.append(exc)

    threads = [threading.Thread(target=writer, args=(i,)) for i in range(20)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    assert not errors
    loaded = load_entry(root, "ns", "same-key")
    assert isinstance(loaded, dict)
    assert isinstance(loaded.get("value"), int)
