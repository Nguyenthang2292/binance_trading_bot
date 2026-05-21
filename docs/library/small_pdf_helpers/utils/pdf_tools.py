from __future__ import annotations

# pyright: reportMissingImports=false, reportMissingTypeStubs=false

import logging
import os
from pathlib import Path
from typing import Any as TypingAny, Callable, cast

from pypdf import PdfWriter, PdfReader

from .pdf_gemini_repair_runner import repair_with_gemini

ProgressCallback = Callable[[int, int], None]

_logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Marker model singleton — loaded once per process
# ---------------------------------------------------------------------------

_marker_models: dict | None = None


def _load_symbol(import_statement: str, symbol_name: str) -> TypingAny:
    namespace: dict[str, TypingAny] = {}
    exec(import_statement, namespace)
    return namespace[symbol_name]


def _get_marker_model_dir() -> Path:
    raw = os.environ.get("MARKER_MODEL_DIR", "./models")
    return Path(raw).expanduser().resolve()


def configure_marker_env() -> None:
    """Set HF_HOME to the local models directory before any marker import.

    Idempotent — safe to call multiple times.
    Creates the directory if it does not exist.
    Default: ./models (relative to CWD at app startup).

    Must be called before any import of marker/surya/transformers modules.
    Call at the top of main() before tk.Tk().
    """
    model_dir = _get_marker_model_dir()
    model_dir.mkdir(parents=True, exist_ok=True)
    os.environ["HF_HOME"] = str(model_dir)


def _get_marker_models() -> dict:
    """Load marker models on first call, return cached dict on subsequent calls.

    create_model_dict() takes 30-120s on first run (downloads ~GB of models).
    The singleton ensures users pay this cost only once per process.
    All marker imports are lazy so HF_HOME is guaranteed to be set first.
    """
    global _marker_models
    if _marker_models is None:
        try:
            create_model_dict = cast(
                TypingAny,
                _load_symbol(
                "from marker.models import create_model_dict",
                "create_model_dict",
                ),
            )
        except ImportError as error:
            raise RuntimeError(
                "Không thể tải marker. Hãy cài đặt: pip install marker-pdf"
            ) from error
        _marker_models = create_model_dict()
    assert _marker_models is not None
    return _marker_models


# ---------------------------------------------------------------------------
# PDF page range extraction (unchanged)
# ---------------------------------------------------------------------------


def resolve_pdf_range_output_path(output_dir: str, input_pdf: str, start_page: int, end_page: int) -> Path:
    input_path = Path(input_pdf)
    output_directory = Path(output_dir).expanduser().resolve()
    base_name = f"{input_path.stem}_pages_{start_page}_{end_page}"
    candidate = output_directory / f"{base_name}.pdf"
    counter = 1

    while candidate.exists():
        candidate = output_directory / f"{base_name}_{counter}.pdf"
        counter += 1

    return candidate


def extract_pdf_page_range(input_pdf: str, start_page: int, end_page: int, output_dir: str) -> str:
    input_path = Path(input_pdf).expanduser().resolve()
    output_directory = Path(output_dir).expanduser().resolve()

    if not input_path.is_file():
        raise ValueError("File PDF nguồn không tồn tại.")

    if start_page <= 0 or end_page <= 0:
        raise ValueError("Số trang phải lớn hơn 0.")

    if start_page > end_page:
        raise ValueError("Số trang đầu không được lớn hơn số trang cuối.")

    if not output_directory.is_dir():
        raise ValueError("Thư mục lưu không tồn tại.")

    if not os.access(output_directory, os.W_OK):
        raise ValueError("Thư mục lưu không có quyền ghi.")

    reader = PdfReader(str(input_path))
    total_pages = len(reader.pages)

    if end_page > total_pages:
        raise ValueError(f"File PDF chỉ có {total_pages} trang.")

    output_path = resolve_pdf_range_output_path(str(output_directory), str(input_path), start_page, end_page)
    writer = PdfWriter()

    for page_number in range(start_page - 1, end_page):
        writer.add_page(reader.pages[page_number])

    with output_path.open("wb") as output_file:
        writer.write(output_file)

    return str(output_path)


# ---------------------------------------------------------------------------
# Markdown output path resolution
# ---------------------------------------------------------------------------


def resolve_markdown_output_path(output_dir: str, input_pdf: str) -> Path:
    input_path = Path(input_pdf)
    output_directory = Path(output_dir).expanduser().resolve()
    candidate = output_directory / f"{input_path.stem}.md"
    counter = 1

    while candidate.exists():
        candidate = output_directory / f"{input_path.stem}_{counter}.md"
        counter += 1

    return candidate


# ---------------------------------------------------------------------------
# Progress helpers
# ---------------------------------------------------------------------------


def _emit_progress(progress_callback: ProgressCallback | None, processed: int, total: int) -> None:
    if progress_callback is None:
        return
    try:
        progress_callback(processed, total)
    except Exception:
        return


def _detect_vram_gb() -> float:
    """Trả về VRAM GPU đầu tiên (GB). Trả 0.0 nếu không có GPU hoặc torch lỗi."""
    try:
        import torch  # type: ignore[import-not-found]  # noqa: PLC0415

        if torch.cuda.is_available():
            return torch.cuda.get_device_properties(0).total_memory / (1024**3)
    except Exception:
        pass
    return 0.0


def _get_marker_config(vram_gb: float) -> dict:
    """Trả dict config cho PdfConverter dựa trên VRAM khả dụng."""
    if vram_gb >= 12:
        return {"batch_multiplier": 4, "workers": 4}
    if vram_gb >= 6:
        return {"batch_multiplier": 2, "workers": 4}
    return {"batch_multiplier": 1, "workers": 2}


# ---------------------------------------------------------------------------
# PDF → Markdown conversion via marker
# ---------------------------------------------------------------------------


def convert_pdf_to_markdown(
    input_pdf: str,
    output_dir: str,
    progress_callback: ProgressCallback | None = None,
) -> str:
    """Convert a PDF file to Markdown using marker-pdf (GPU-accelerated).

    Progress callback receives exactly two calls: (0, 1) at start and (1, 1) at end.
    Output is written directly from marker — Markdown structure (headings, tables,
    lists) is preserved without further post-processing.

    Raises:
        ValueError: If input_pdf or output_dir is invalid.
        RuntimeError: If marker-pdf is not installed or models cannot be loaded.
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

    # Signal start — UI switches progress bar to indeterminate mode
    _emit_progress(progress_callback, 0, 1)

    try:
        PdfConverter = cast(
            TypingAny,
            _load_symbol("from marker.converters.pdf import PdfConverter", "PdfConverter"),
        )
        text_from_rendered = cast(
            TypingAny,
            _load_symbol("from marker.output import text_from_rendered", "text_from_rendered"),
        )
    except ImportError as error:
        raise RuntimeError(
            "Không thể tải marker. Hãy cài đặt: pip install marker-pdf"
        ) from error

    # TODO: log stage "Đang tải models marker..." (hint: dùng _logger.info)
    models = _get_marker_models()

    vram_gb = _detect_vram_gb()
    marker_config = _get_marker_config(vram_gb)
    # TODO: log vram_gb và batch_multiplier đã chọn

    try:
        converter = PdfConverter(artifact_dict=models, config=marker_config)
    except TypeError:
        # Fallback nếu version marker không nhận config keyword
        converter = PdfConverter(artifact_dict=models)

    # TODO: log stage "Đang chạy marker trên: <tên file>" (hint: input_path.name)
    rendered = cast(TypingAny, converter(str(input_path)))  # pyright: ignore[reportCallIssue]
    text, _metadata, _images = cast(tuple[TypingAny, TypingAny, TypingAny], text_from_rendered(rendered))  # pyright: ignore[reportCallIssue]
    text = cast(str, text)

    # Write output directly — marker produces well-formed Markdown
    # Steps 5–6 execute unconditionally even when marker returns empty text
    if not text or not text.strip():
        text = ""

    output_path.write_text(text, encoding="utf-8")

    # TODO: log stage "Marker xong. Chạy Gemini repair..."
    repair_with_gemini(str(input_path), output_path, progress_callback=None)

    # Signal done — UI stops indeterminate, shows 100%
    _emit_progress(progress_callback, 1, 1)
    return str(output_path)


__all__ = [
    "configure_marker_env",
    "convert_pdf_to_markdown",
    "extract_pdf_page_range",
    "resolve_markdown_output_path",
    "resolve_pdf_range_output_path",
]
