#!/usr/bin/env python3
"""Build docs/library/index.json from 00-book-references.md files."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple


ROOT = Path(__file__).resolve().parents[1]
INDEX_PATH = ROOT / "docs" / "library" / "index.json"
GENERATOR = "tools/build_library_index.py"
VERSION = "1.0"

BOOK_ID_RE = re.compile(r"^[a-z0-9-]+$")
SOURCE_REF_RE = re.compile(
    r"^\*\*Source ref:\*\*[ \t]+`(?P<book_id>[a-z0-9-]+)`(?:[ \t]+\u2014[ \t]+(?P<note>.+))?[ \t]*$",
    re.MULTILINE,
)
IDENTIFIER_KEYS = ("isbn_hardback", "isbn_pdf", "isbn_epub", "lccn", "doi", "url")
REQUIRED_SOURCE_FIELDS = ("book_id", "type", "title", "authors", "year")
VALID_TYPES = {"book", "paper", "online"}


def rel(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def strip_inline_comment(value: str) -> str:
    quote: Optional[str] = None
    escaped = False
    for index, char in enumerate(value):
        if escaped:
            escaped = False
            continue
        if char == "\\":
            escaped = True
            continue
        if quote:
            if char == quote:
                quote = None
            continue
        if char in {"'", '"'}:
            quote = char
            continue
        if char == "#":
            return value[:index].rstrip()
    return value.strip()


def parse_scalar(raw: str) -> Any:
    value = strip_inline_comment(raw)
    if value == "":
        return ""
    if value in {"null", "Null", "NULL", "~"}:
        return None
    if value == "[]":
        return []
    if value in {"true", "True", "TRUE"}:
        return True
    if value in {"false", "False", "FALSE"}:
        return False
    if (value.startswith('"') and value.endswith('"')) or (
        value.startswith("'") and value.endswith("'")
    ):
        return value[1:-1]
    if re.fullmatch(r"-?\d+", value):
        return int(value)
    return value


def split_key_value(line: str) -> Tuple[str, str]:
    if ":" not in line:
        raise ValueError(f"expected key/value line: {line!r}")
    key, value = line.split(":", 1)
    return key.strip(), value.strip()


def parse_list(block: List[str]) -> List[Any]:
    items: List[Any] = []
    index = 0
    while index < len(block):
        line = block[index]
        stripped = line.strip()
        if not stripped:
            index += 1
            continue
        if not stripped.startswith("- "):
            raise ValueError(f"expected list item: {line!r}")

        item_text = stripped[2:].strip()
        if item_text and ":" in item_text:
            key, value = split_key_value(item_text)
            item: Dict[str, Any] = {key: parse_scalar(value)}
            index += 1
            while index < len(block):
                child = block[index]
                child_stripped = child.strip()
                if not child_stripped:
                    index += 1
                    continue
                if child_stripped.startswith("- "):
                    break
                child_key, child_value = split_key_value(child_stripped)
                item[child_key] = parse_scalar(child_value)
                index += 1
            items.append(item)
            continue

        items.append(parse_scalar(item_text))
        index += 1
    return items


def parse_object(block: List[str]) -> Dict[str, Any]:
    obj: Dict[str, Any] = {}
    for line in block:
        stripped = line.strip()
        if not stripped:
            continue
        key, value = split_key_value(stripped)
        obj[key] = parse_scalar(value)
    return obj


def parse_yaml_subset(lines: List[str]) -> Dict[str, Any]:
    data: Dict[str, Any] = {}
    index = 0
    while index < len(lines):
        line = lines[index]
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            index += 1
            continue
        if line.startswith((" ", "\t")):
            raise ValueError(f"unexpected indentation at top level: {line!r}")

        key, value = split_key_value(line)
        if value:
            data[key] = parse_scalar(value)
            index += 1
            continue

        index += 1
        block: List[str] = []
        while index < len(lines):
            child = lines[index]
            if child.strip() and not child.startswith((" ", "\t")):
                break
            block.append(child)
            index += 1

        first = next((entry.strip() for entry in block if entry.strip()), "")
        data[key] = parse_list(block) if first.startswith("- ") else parse_object(block)
    return data


def extract_frontmatter(path: Path) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        return None, "missing YAML frontmatter"

    end_index = None
    for index in range(1, len(lines)):
        if lines[index].strip() == "---":
            end_index = index
            break
    if end_index is None:
        return None, "unterminated YAML frontmatter"

    try:
        return parse_yaml_subset(lines[1:end_index]), None
    except ValueError as exc:
        return None, str(exc)


def normalize_entry(path: Path, raw: Dict[str, Any]) -> Tuple[Optional[Dict[str, Any]], List[str], List[str]]:
    errors: List[str] = []
    warnings: List[str] = []
    path_label = rel(path)

    for field in REQUIRED_SOURCE_FIELDS:
        if field not in raw or raw[field] in (None, ""):
            errors.append(f"{path_label}: missing required field '{field}'")

    book_id = raw.get("book_id")
    source_type = raw.get("type")
    authors = raw.get("authors")
    year = raw.get("year")

    if book_id is not None and not isinstance(book_id, str):
        errors.append(f"{path_label}: field 'book_id' must be a string")
    elif isinstance(book_id, str) and not BOOK_ID_RE.fullmatch(book_id):
        errors.append(f"{path_label}: invalid book_id '{book_id}'")

    if source_type is not None and source_type not in VALID_TYPES:
        errors.append(f"{path_label}: invalid type '{source_type}'")

    if authors is not None and (
        not isinstance(authors, list)
        or not authors
        or not all(isinstance(author, str) and author for author in authors)
    ):
        errors.append(f"{path_label}: field 'authors' must be a non-empty string array")

    if year is not None and not isinstance(year, int):
        errors.append(f"{path_label}: field 'year' must be an integer")

    raw_identifiers = raw.get("identifiers") or {}
    if not isinstance(raw_identifiers, dict):
        errors.append(f"{path_label}: field 'identifiers' must be an object")
        raw_identifiers = {}

    for unknown in sorted(set(raw_identifiers) - set(IDENTIFIER_KEYS)):
        warnings.append(f"{path_label}: ignoring unsupported identifier '{unknown}'")

    raw_implemented = raw.get("implemented_chapters") or []
    if not isinstance(raw_implemented, list):
        errors.append(f"{path_label}: field 'implemented_chapters' must be an array")
        raw_implemented = []

    implemented_in: List[Dict[str, str]] = []
    for item_index, item in enumerate(raw_implemented):
        if not isinstance(item, dict):
            errors.append(f"{path_label}: implemented_chapters[{item_index}] must be an object")
            continue
        file_value = item.get("file")
        if not isinstance(file_value, str) or not file_value:
            errors.append(f"{path_label}: implemented_chapters[{item_index}] missing required field 'file'")
            continue
        normalized_item = {"file": file_value}
        chapter = item.get("chapter")
        if isinstance(chapter, str) and chapter:
            normalized_item["chapter"] = chapter
        implemented_in.append(normalized_item)

        target = ROOT / file_value
        if not target.exists():
            warnings.append(f"file not found: {file_value}")

    if errors:
        return None, errors, warnings

    entry = {
        "book_id": book_id,
        "type": source_type,
        "title": raw["title"],
        "authors": authors,
        "year": year,
        "publisher": raw.get("publisher"),
        "identifiers": {key: raw_identifiers.get(key) for key in IDENTIFIER_KEYS},
        "purchase_url": raw.get("purchase_url"),
        "implemented_in": sorted(
            implemented_in,
            key=lambda item: (item["file"], item.get("chapter", "")),
        ),
        "source_file": path_label,
    }
    return entry, errors, warnings


def strategy_paths() -> List[Path]:
    strategies_root = ROOT / "docs" / "strategies"
    if not strategies_root.exists():
        return []
    paths = []
    for path in strategies_root.rglob("*.md"):
        if path.name == "00-book-references.md":
            continue
        if path.name.startswith("pass-"):
            continue
        paths.append(path)
    return sorted(paths)


def source_refs(path: Path) -> Set[str]:
    text = path.read_text(encoding="utf-8")
    return {match.group("book_id") for match in SOURCE_REF_RE.finditer(text)}


def validate_cross_refs(entries: List[Dict[str, Any]]) -> Tuple[List[str], List[str]]:
    errors: List[str] = []
    warnings: List[str] = []
    book_ids = {entry["book_id"] for entry in entries}
    implemented_by_book = {
        entry["book_id"]: {item["file"] for item in entry["implemented_in"]}
        for entry in entries
    }

    strategy_refs: Dict[str, Set[str]] = {}
    for path in strategy_paths():
        refs = source_refs(path)
        if not refs:
            continue
        path_label = rel(path)
        strategy_refs[path_label] = refs
        for book_id in sorted(refs):
            if book_id not in book_ids:
                errors.append(f"{path_label}: unknown source ref: {book_id}")
                continue
            if path_label not in implemented_by_book[book_id]:
                errors.append(
                    f"{path_label}: source ref missing from implemented_chapters for {book_id}"
                )

    for entry in entries:
        book_id = entry["book_id"]
        for item in entry["implemented_in"]:
            file_value = item["file"]
            target = ROOT / file_value
            if not target.exists():
                warnings.append(f"file not found: {file_value}")
                continue
            refs = strategy_refs.get(file_value)
            if refs is None:
                refs = source_refs(target)
            if book_id not in refs:
                errors.append(f"{file_value}: implemented file missing source ref: {book_id}")

    return errors, warnings


def build_index() -> int:
    errors: List[str] = []
    warnings: List[str] = []
    entries: List[Dict[str, Any]] = []
    seen: Dict[str, str] = {}

    source_files = sorted((ROOT / "docs").rglob("00-book-references.md"))
    for path in source_files:
        raw, frontmatter_error = extract_frontmatter(path)
        if raw is None:
            warnings.append(f"{rel(path)}: {frontmatter_error}; skipped")
            continue

        entry, entry_errors, entry_warnings = normalize_entry(path, raw)
        errors.extend(entry_errors)
        warnings.extend(entry_warnings)
        if entry is None:
            continue

        book_id = entry["book_id"]
        if book_id in seen:
            errors.append(f"{rel(path)}: duplicate book_id '{book_id}' also in {seen[book_id]}")
            continue
        seen[book_id] = rel(path)
        entries.append(entry)
        print(f"[OK]  Processed: {rel(path)}")

    entries.sort(key=lambda entry: entry["book_id"])
    cross_errors, cross_warnings = validate_cross_refs(entries)
    errors.extend(cross_errors)
    warnings.extend(cross_warnings)

    for warning in warnings:
        print(f"[WARN] {warning}", file=sys.stderr)
    if errors:
        for error in errors:
            print(f"[ERR] {error}", file=sys.stderr)
        return 1

    index = {
        "generator": GENERATOR,
        "version": VERSION,
        "entries": entries,
    }
    content = json.dumps(index, ensure_ascii=False, indent=2) + "\n"
    INDEX_PATH.parent.mkdir(parents=True, exist_ok=True)
    if INDEX_PATH.exists() and INDEX_PATH.read_text(encoding="utf-8") == content:
        print(f"[OK]  Unchanged: {rel(INDEX_PATH)} ({len(entries)} entries)")
    else:
        INDEX_PATH.write_text(content, encoding="utf-8")
        print(f"[OK]  Written: {rel(INDEX_PATH)} ({len(entries)} entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(build_index())
