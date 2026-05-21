from __future__ import annotations

import os
from pathlib import Path
from typing import Callable

try:
    from .gemini_key_manager import has_gemini_key_in_env
    from .gemini_vision_client import GeminiVisionClient
    from .pdf_tools import resolve_markdown_output_path
except ImportError:
    from gemini_key_manager import has_gemini_key_in_env
    from gemini_vision_client import GeminiVisionClient
    from pdf_tools import resolve_markdown_output_path

ProgressCallback = Callable[[int, int], None]


def _emit_progress(
    progress_callback: ProgressCallback | None,
    processed: int,
    total: int,
) -> None:
    if progress_callback is None:
        return
    try:
        progress_callback(processed, total)
    except Exception:
        return


def _render_page(doc: object, page_index: int, dpi: int = 150) -> object:
    try:
        import fitz  # noqa: PLC0415
        from PIL import Image  # noqa: PLC0415
    except ImportError as error:
        raise RuntimeError("Cannot import pymupdf. Install with: pip install pymupdf") from error

    page = doc[page_index]  # type: ignore[index]
    mat = fitz.Matrix(dpi / 72, dpi / 72)
    pix = page.get_pixmap(matrix=mat)
    return Image.frombytes("RGB", [pix.width, pix.height], pix.samples)


def convert_pdf_to_markdown_gemini(
    input_pdf: str,
    output_dir: str,
    progress_callback: ProgressCallback | None = None,
) -> str:
    input_path = Path(input_pdf).expanduser().resolve()
    output_directory = Path(output_dir).expanduser().resolve()

    if not has_gemini_key_in_env():
        raise ValueError(
            "Missing Gemini API key. Set one of: GEMINI_API_KEY_1..N, GEMINI_API_KEYS, or GEMINI_API_KEY."
        )

    if not input_path.is_file():
        raise ValueError("Input PDF file does not exist.")

    if not output_directory.is_dir():
        raise ValueError("Output directory does not exist.")

    if not os.access(output_directory, os.W_OK):
        raise ValueError("Output directory is not writable.")

    try:
        import fitz  # noqa: PLC0415
    except ImportError as error:
        raise RuntimeError("Cannot import pymupdf. Install with: pip install pymupdf") from error

    client = GeminiVisionClient.from_environment()
    output_path = resolve_markdown_output_path(str(output_directory), str(input_path))

    doc = fitz.open(str(input_path))
    total_pages = len(doc)

    _emit_progress(progress_callback, 0, total_pages)

    sections: list[str] = []
    error_count = 0

    for page_index in range(total_pages):
        try:
            image = _render_page(doc, page_index)
            text = client.extract_full_page(image)
            sections.append(text)
        except Exception as error:
            sections.append(f"<!-- Gemini error: page {page_index + 1} - {error} -->")
            error_count += 1

        _emit_progress(progress_callback, page_index + 1, total_pages)

    doc.close()

    if error_count == total_pages:
        raise RuntimeError(
            f"Gemini failed to extract content from all {total_pages} pages."
        )

    markdown = "\n\n---\n\n".join(sections)
    output_path.write_text(markdown, encoding="utf-8")

    return str(output_path)


__all__ = ["convert_pdf_to_markdown_gemini"]

