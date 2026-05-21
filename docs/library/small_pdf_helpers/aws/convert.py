from __future__ import annotations

import os
from pathlib import Path


def _first_pdf(input_dir: Path) -> Path:
    files = sorted(input_dir.glob("*.pdf"))
    if not files:
        raise FileNotFoundError(f"No PDF file found in {input_dir}")
    return files[0]


def main() -> None:
    input_dir = Path(os.environ.get("SM_CHANNEL_INPUT", "/opt/ml/processing/input"))
    output_dir = Path(os.environ.get("SM_CHANNEL_OUTPUT", "/opt/ml/processing/output"))
    output_dir.mkdir(parents=True, exist_ok=True)

    input_pdf = _first_pdf(input_dir)
    print(f"[convert] Input PDF: {input_pdf}", flush=True)

    try:
        from pypdf import PdfReader
        pdf_page_count = len(PdfReader(str(input_pdf)).pages)
        print(f"[convert] PDF page count (pypdf): {pdf_page_count}", flush=True)
    except Exception as exc:
        print(f"[convert] Could not count PDF pages: {exc}", flush=True)
        pdf_page_count = None

    from marker.converters.pdf import PdfConverter
    from marker.models import create_model_dict
    from marker.output import text_from_rendered

    converter = PdfConverter(artifact_dict=create_model_dict())
    rendered = converter(str(input_pdf))

    # Diagnostic: count how many pages marker actually rendered
    rendered_pages = (
        getattr(rendered, "pages", None)
        or getattr(rendered, "children", None)
        or []
    )
    print(f"[convert] Marker rendered pages: {len(rendered_pages)}", flush=True)
    if pdf_page_count and len(rendered_pages) < pdf_page_count:
        print(
            f"[convert] WARNING: marker only rendered {len(rendered_pages)}"
            f" of {pdf_page_count} pages — possible truncation!",
            flush=True,
        )

    text, _metadata, _images = text_from_rendered(rendered)
    char_count = len(text or "")
    line_count = (text or "").count("\n")
    print(
        f"[convert] Output text: {char_count} chars, {line_count} lines",
        flush=True,
    )

    output_path = output_dir / f"{input_pdf.stem}.md"
    output_path.write_text(text or "", encoding="utf-8")
    print(f"[convert] Written: {output_path} ({output_path.stat().st_size} bytes)", flush=True)


if __name__ == "__main__":
    main()
