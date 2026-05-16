from __future__ import annotations

from pathlib import Path

from tools.gemini_filter.cache_store import load_entry, save_entry


def test_save_and_load_entry(tmp_path: Path) -> None:
    root = tmp_path / "root"
    save_entry(root, "ns", "k1", {"value": 123})
    loaded = load_entry(root, "ns", "k1")
    assert loaded == {"value": 123}


def test_load_missing_entry_returns_none(tmp_path: Path) -> None:
    loaded = load_entry(tmp_path / "root", "ns", "missing")
    assert loaded is None

