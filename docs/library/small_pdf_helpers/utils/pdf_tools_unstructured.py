from __future__ import annotations

import os
from pathlib import Path
from typing import Callable

try:
    from .pdf_tools import resolve_markdown_output_path
except ImportError:
    from pdf_tools import resolve_markdown_output_path

ProgressCallback = Callable[[int, int], None]


def _emit_progress(progress_callback: ProgressCallback | None, processed: int, total: int) -> None:
    if progress_callback is None:
        return
    try:
        progress_callback(processed, total)
    except Exception:
        return


def convert_pdf_to_markdown(
    input_pdf: str,
    output_dir: str,
    progress_callback: ProgressCallback | None = None,
) -> str:
    """Convert PDF to Markdown using Unstructured with strategy='fast'.

    This flow is CPU-friendly and does not require GPU/detectron2.
    """
    input_path = Path(input_pdf).expanduser().resolve()
    output_directory = Path(output_dir).expanduser().resolve()

    if not input_path.is_file():
        raise ValueError("File PDF nguồn không tồn tại.")

    if not output_directory.is_dir():
        raise ValueError("Thư mục lưu không tồn tại.")

    if not os.access(output_directory, os.W_OK):
        raise ValueError("Thư mục lưu không có quyền ghi.")

    output_path = resolve_markdown_output_path(str(output_directory), str(input_path))

    _emit_progress(progress_callback, 0, 1)

    try:
        from unstructured.chunking.title import chunk_by_title  # noqa: PLC0415
        from unstructured.partition.pdf import partition_pdf  # noqa: PLC0415
    except ImportError as error:
        raise RuntimeError(
            'Không thể tải unstructured. Hãy cài đặt: pip install "unstructured[pdf]"'
        ) from error

    try:
        elements = partition_pdf(filename=str(input_path), strategy="fast")
    except Exception as error:
        raise RuntimeError(f"Unstructured failed to parse PDF: {error}") from error

    if not elements:
        raise ValueError(
            "Không trích xuất được nội dung từ PDF. "
            "File có thể là PDF scan (ảnh, không có text layer). "
            "Hãy dùng option 'Local (marker)' hoặc 'AWS' thay thế."
        )

    chunks = chunk_by_title(elements)
    sections: list[str] = []
    for chunk in chunks:
        text = getattr(chunk, "text", "")
        if isinstance(text, str) and text.strip():
            sections.append(text.strip())

    if not sections:
        raise ValueError(
            "Không trích xuất được nội dung từ PDF. "
            "File có thể là PDF scan (ảnh, không có text layer). "
            "Hãy dùng option 'Local (marker)' hoặc 'AWS' thay thế."
        )

    markdown = "\n\n---\n\n".join(sections)
    output_path.write_text(markdown, encoding="utf-8")

    _emit_progress(progress_callback, 1, 1)
    return str(output_path)


__all__ = ["convert_pdf_to_markdown"]

