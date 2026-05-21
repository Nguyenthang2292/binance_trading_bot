from __future__ import annotations

import re
from pathlib import Path
from typing import Any, Callable

try:
    from pdf2image import convert_from_path  # type: ignore[import-not-found]
except ImportError:  # pragma: no cover - patched in tests when dependency is absent
    def convert_from_path(*args: Any, **kwargs: Any) -> list[Any]:
        raise ImportError("pdf2image is required for hexagram repair")


IMAGE_REF_RE = re.compile(r"!\[.*?\]\(_page_(\d+)_Picture_\d+\.\w+\)")
BROKEN_TABLE_RE = re.compile(r"\|[^\n]*\*{4,}[^\n]*\|")
NO_HEXAGRAM_TABLE = "NO_HEXAGRAM_TABLE"
FAIL_COMMENT_TEMPLATE = "<!-- GEMINI_FIX_FAILED: page {page_1idx} -->"
ProgressCallback = Callable[[str], None]


def _safe_progress(progress_cb: ProgressCallback | None, message: str) -> None:
    if progress_cb is None:
        return
    try:
        progress_cb(message)
    except Exception:
        return


def _line_starts(md_text: str) -> list[tuple[int, str]]:
    lines = md_text.splitlines(keepends=True)
    starts: list[tuple[int, str]] = []
    offset = 0
    for line in lines:
        starts.append((offset, line))
        offset += len(line)
    return starts


def _page_start_line(lines: list[str], page_idx: int) -> int | None:
    for idx, line in enumerate(lines):
        match = IMAGE_REF_RE.search(line)
        if match and int(match.group(1)) == page_idx:
            return idx
    return None


def _broken_table_start_line(lines: list[str], page_idx: int) -> int | None:
    current_page: int | None = None
    for idx, line in enumerate(lines):
        match = IMAGE_REF_RE.search(line)
        if match:
            current_page = int(match.group(1))
        if current_page == page_idx and BROKEN_TABLE_RE.search(line):
            return idx
    return None


def _page_block_bounds(md_text: str, page_idx: int) -> tuple[int, int] | None:
    line_meta = _line_starts(md_text)
    plain_lines = [line for _, line in line_meta]

    start_line = _page_start_line(plain_lines, page_idx)
    if start_line is None:
        start_line = _broken_table_start_line(plain_lines, page_idx)
    if start_line is None:
        return None

    end_line = len(plain_lines)
    for idx in range(start_line + 1, len(plain_lines)):
        line = plain_lines[idx]
        match = IMAGE_REF_RE.search(line)
        if match and int(match.group(1)) != page_idx:
            end_line = idx
            break
        if not line.strip() and idx + 1 < len(plain_lines) and not plain_lines[idx + 1].strip():
            end_line = idx
            break

    start_pos = line_meta[start_line][0]
    end_pos = line_meta[end_line][0] if end_line < len(line_meta) else len(md_text)
    return start_pos, end_pos


def detect_pages_to_repair(md_text: str) -> set[int]:
    """Return 0-indexed page numbers that have broken hexagram tables."""
    pages: set[int] = set()
    current_page: int | None = None

    for line in md_text.splitlines():
        match = IMAGE_REF_RE.search(line)
        if match:
            current_page = int(match.group(1))
            continue  # track page boundary only; add only when broken table found

        if current_page is not None and BROKEN_TABLE_RE.search(line):
            pages.add(current_page)

    return pages


def replace_page_block(md_text: str, page_idx: int, gemini_md: str) -> str:
    """Replace one page block with Gemini output, skipping no-table markers."""
    if gemini_md.strip() == NO_HEXAGRAM_TABLE:
        return md_text

    bounds = _page_block_bounds(md_text, page_idx)
    if bounds is None:
        return md_text

    start_pos, end_pos = bounds
    replacement = gemini_md.strip()
    if end_pos < len(md_text) and md_text[end_pos] not in "\r\n":
        replacement += "\n\n"
    return md_text[:start_pos] + replacement + md_text[end_pos:]


def insert_fail_comment(md_text: str, page_idx: int, page_1idx: int) -> str:
    """Insert a best-effort failure marker for one PDF page."""
    comment = FAIL_COMMENT_TEMPLATE.format(page_1idx=page_1idx)
    bounds = _page_block_bounds(md_text, page_idx)
    if bounds is None:
        return md_text.rstrip() + "\n" + comment + "\n"

    start_pos, _ = bounds
    return md_text[:start_pos] + comment + "\n" + md_text[start_pos:]


def repair_hexagram_tables(
    md_path: Path,
    pdf_path: Path,
    client: Any,
    progress_cb: ProgressCallback | None = None,
) -> None:
    """Repair hexagram tables in a Markdown file using rendered PDF pages."""
    md_text = md_path.read_text(encoding="utf-8")
    pages_0idx = detect_pages_to_repair(md_text)
    if not pages_0idx:
        md_path.write_text(md_text, encoding="utf-8")
        return

    pages_1idx = sorted(page + 1 for page in pages_0idx)
    try:
        images = convert_from_path(
            str(pdf_path),
            dpi=200,
            first_page=pages_1idx[0],
            last_page=pages_1idx[-1],
        )
    except Exception as exc:
        _safe_progress(progress_cb, f"Gemini repair skipped: {exc}")
        md_path.write_text(md_text, encoding="utf-8")
        return

    base_page = pages_1idx[0]
    for page_1idx in pages_1idx:
        page_0idx = page_1idx - 1
        image_index = page_1idx - base_page
        if image_index < 0 or image_index >= len(images):
            md_text = insert_fail_comment(md_text, page_0idx, page_1idx)
            continue

        image = images[image_index]
        _safe_progress(progress_cb, f"Repairing page {page_1idx}...")
        try:
            gemini_md = client.extract_hexagram_table(image)
            if gemini_md.strip() == NO_HEXAGRAM_TABLE:
                _safe_progress(progress_cb, f"Skipped page {page_1idx}: no hexagram table.")
                continue

            md_text = replace_page_block(md_text, page_0idx, gemini_md)
            _safe_progress(progress_cb, f"Repaired page {page_1idx}.")
        except Exception:
            md_text = insert_fail_comment(md_text, page_0idx, page_1idx)
            _safe_progress(progress_cb, f"Failed to repair page {page_1idx}.")

    md_path.write_text(md_text, encoding="utf-8")


__all__ = [
    "detect_pages_to_repair",
    "insert_fail_comment",
    "repair_hexagram_tables",
    "replace_page_block",
]
