from __future__ import annotations

import json
from pathlib import Path
from typing import Optional

import pytest

from tools import build_library_index


def _configure_workspace(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    index_path = tmp_path / "docs" / "library" / "index.json"
    monkeypatch.setattr(build_library_index, "ROOT", tmp_path)
    monkeypatch.setattr(build_library_index, "INDEX_PATH", index_path)
    return index_path


def _write_source(
    root: Path,
    *,
    book_id: str = "penfold-2020-universal-tactics",
    title: str = "The Universal Tactics of Successful Trend Trading",
    authors: Optional[str] = '  - "Brent Norman Lindsay Penfold"',
    year: str = "2020",
    implemented: str = '  - chapter: "Chapter 7 - Golden 50/200"\n    file: "docs/strategies/golden_crossover.md"',
) -> Path:
    path = root / "docs" / "strategies" / book_id / "00-book-references.md"
    path.parent.mkdir(parents=True, exist_ok=True)
    authors_section = f"authors:\n{authors}\n" if authors is not None else ""
    implemented_section = (
        "implemented_chapters: []"
        if implemented == "[]"
        else f"implemented_chapters:\n{implemented}"
    )
    path.write_text(
        f"""---
book_id: "{book_id}"
type: "book"
title: "{title}"
{authors_section}year: {year}
publisher: "Wiley"
identifiers:
  isbn_hardback: "9781119734512"
  isbn_pdf: null
  isbn_epub: null
  lccn: null
  doi: null
  url: null
purchase_url: "https://www.amazon.com/dp/1119734517"
{implemented_section}
---

# {title}
""",
        encoding="utf-8",
    )
    return path


def _write_strategy(root: Path, book_id: str = "penfold-2020-universal-tactics") -> Path:
    path = root / "docs" / "strategies" / "golden_crossover.md"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        f"""# Golden 50/200 Moving Average Crossover

**Type:** `golden_crossover`
**Version:** 1.0
**Date:** 2026-05-16
**Source ref:** `{book_id}` — Chapter 7, "Golden 50 and 200-Day Crossover"

---
""",
        encoding="utf-8",
    )
    return path


def test_build_index_writes_deterministic_index(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    index_path = _configure_workspace(tmp_path, monkeypatch)
    _write_source(tmp_path)
    _write_strategy(tmp_path)

    assert build_library_index.build_index() == 0

    index = json.loads(index_path.read_text(encoding="utf-8"))
    assert index == {
        "generator": "tools/build_library_index.py",
        "version": "1.0",
        "entries": [
            {
                "book_id": "penfold-2020-universal-tactics",
                "type": "book",
                "title": "The Universal Tactics of Successful Trend Trading",
                "authors": ["Brent Norman Lindsay Penfold"],
                "year": 2020,
                "publisher": "Wiley",
                "identifiers": {
                    "isbn_hardback": "9781119734512",
                    "isbn_pdf": None,
                    "isbn_epub": None,
                    "lccn": None,
                    "doi": None,
                    "url": None,
                },
                "purchase_url": "https://www.amazon.com/dp/1119734517",
                "implemented_in": [
                    {
                        "file": "docs/strategies/golden_crossover.md",
                        "chapter": "Chapter 7 - Golden 50/200",
                    }
                ],
                "source_file": "docs/strategies/penfold-2020-universal-tactics/00-book-references.md",
            }
        ],
    }
    assert "generated_at" not in index

    first_content = index_path.read_text(encoding="utf-8")
    assert build_library_index.build_index() == 0
    assert index_path.read_text(encoding="utf-8") == first_content


def test_missing_required_field_fails(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]) -> None:
    _configure_workspace(tmp_path, monkeypatch)
    _write_source(tmp_path, authors=None)
    _write_strategy(tmp_path)

    assert build_library_index.build_index() == 1
    captured = capsys.readouterr()
    assert "missing required field 'authors'" in captured.err


def test_duplicate_book_id_fails(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]) -> None:
    _configure_workspace(tmp_path, monkeypatch)
    _write_source(tmp_path)
    duplicate = tmp_path / "docs" / "more" / "00-book-references.md"
    duplicate.parent.mkdir(parents=True, exist_ok=True)
    duplicate.write_text(
        """---
book_id: "penfold-2020-universal-tactics"
type: "book"
title: "Duplicate"
authors:
  - "Author"
year: 2021
implemented_chapters: []
---
""",
        encoding="utf-8",
    )

    assert build_library_index.build_index() == 1
    captured = capsys.readouterr()
    assert "duplicate book_id 'penfold-2020-universal-tactics'" in captured.err


def test_strategy_source_ref_requires_implemented_chapter_link(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    _configure_workspace(tmp_path, monkeypatch)
    _write_source(tmp_path, implemented="[]")
    _write_strategy(tmp_path)

    assert build_library_index.build_index() == 1
    captured = capsys.readouterr()
    assert "source ref missing from implemented_chapters" in captured.err


def test_implemented_file_requires_matching_source_ref(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    _configure_workspace(tmp_path, monkeypatch)
    _write_source(tmp_path)
    _write_strategy(tmp_path, book_id="other-2020-source")

    assert build_library_index.build_index() == 1
    captured = capsys.readouterr()
    assert "unknown source ref: other-2020-source" in captured.err
    assert "implemented file missing source ref: penfold-2020-universal-tactics" in captured.err
