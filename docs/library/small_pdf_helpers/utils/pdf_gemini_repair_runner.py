from __future__ import annotations

from pathlib import Path
from typing import Callable

_StatusCallback = Callable[[str], None] | None


def _emit(callback: _StatusCallback, message: str) -> None:
    if callback is None:
        return
    try:
        callback(message)
    except Exception:
        return


def repair_with_gemini(
    input_pdf: str,
    md_path: Path,
    progress_callback: _StatusCallback = None,
) -> None:
    """Best-effort Gemini Vision repair for broken table blocks."""
    try:
        try:
            from .gemini_key_manager import has_gemini_key_in_env  # noqa: PLC0415
            from .gemini_vision_client import GeminiVisionClient  # noqa: PLC0415
            from .pdf_hexagram_repair import repair_hexagram_tables  # noqa: PLC0415
        except ImportError:
            from gemini_key_manager import has_gemini_key_in_env  # noqa: PLC0415
            from gemini_vision_client import GeminiVisionClient  # noqa: PLC0415
            from pdf_hexagram_repair import repair_hexagram_tables  # noqa: PLC0415

        if not has_gemini_key_in_env():
            _emit(progress_callback, "Skip Gemini fix: no Gemini API key configured.")
            return

        client = GeminiVisionClient.from_environment()
        repair_hexagram_tables(
            md_path=md_path,
            pdf_path=Path(input_pdf),
            client=client,
            progress_cb=lambda msg: _emit(progress_callback, msg),
        )
    except Exception as error:
        _emit(progress_callback, f"Gemini fix failed (skipped): {error}")


__all__ = ["repair_with_gemini"]

