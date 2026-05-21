# Gemini Repair Design (Minimal)

## Purpose

Gemini repair la post-processing step de sua cac block bang bi vo sau khi chuyen PDF -> Markdown.
Step nay la best-effort: loi thi bo qua, khong lam fail luong convert chinh.

## Scope hien tai

- Repair logic dung chung cho cac flow co goi `repair_with_gemini(...)`.
- Input:
  - `input_pdf`: file PDF goc.
  - `md_path`: file Markdown da tao.
- Output:
  - Ghi de `md_path` neu co trang can sua.
  - Neu khong co key hoac Gemini loi: giu nguyen output hien co.

## Thanh phan chinh

- `utils/pdf_gemini_repair_runner.py`
  - Entry point: `repair_with_gemini(...)`
  - Check key env.
  - Tao `GeminiVisionClient` va goi `repair_hexagram_tables(...)`.
- `utils/pdf_hexagram_repair.py`
  - Detect trang can repair.
  - Render anh trang va thay block markdown theo tung trang.
- `utils/gemini_vision_client.py`
  - Goi Gemini Vision de lay markdown cho tung trang.
- `utils/gemini_key_manager.py`
  - Ho tro key rotation (round-robin + failover).

## Gemini key strategy

Ho tro theo thu tu uu tien:

1. `GEMINI_API_KEY_1..N`
2. `GEMINI_API_KEYS` (comma/semicolon/newline separated)
3. `GEMINI_API_KEY`
4. `GEMINI_TEXT_API_KEY` (fallback cuoi)

Neu request gap loi retryable (`429`, `quota`, `rate`, `5xx`, key issue), he thong doi sang key tiep theo.

## Runtime requirements

- Python packages: `google-genai`, `pdf2image`, `pillow`.
- Windows can poppler binary (`pdftoppm`) trong `PATH` de `pdf2image` hoat dong.

## Error behavior

- Khong co Gemini key:
  - Log/trang thai: skip repair.
  - Khong raise exception.
- Loi Gemini tren mot so trang:
  - Insert marker fail theo trang (neu co).
  - Tiep tuc cac trang con lai.
- Loi toan bo repair:
  - Keep markdown output hien co.
  - Khong fail convert pipeline.

## Non-goals

- Khong thay doi UI.
- Khong thay doi luong convert PDF -> Markdown chinh.
- Khong ep buoc repair phai thanh cong moi tra ket qua.

