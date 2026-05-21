# Small PDF Helpers (Library Module)

Module nay duoc copy vao:

`docs/library/small_pdf_helpers`

Muc tieu:

- Cat PDF theo khoang trang.
- Chuyen PDF sang Markdown bang Gemini Vision (UI app).
- GUI desktop da refactor sang DearPyGui.

## Chay GUI

```powershell
python docs/library/small_pdf_helpers/main.py
```

Hoac dung entry point o root:

```powershell
python run_small_pdf_helpers.py
```

## Dependencies toi thieu

```powershell
pip install dearpygui python-dotenv pypdf
```

Hoac cai full dependency cho module:

```powershell
pip install -r docs/library/small_pdf_helpers/requirements.txt
```

Neu dung cac mode nang cao:

- Local marker: `marker-pdf`
- AWS: `boto3`
- Unstructured: `unstructured[pdf]`
- Gemini: `google-genai`, `pymupdf`, `pillow`

## Gemini key rotation

Module ho tro round-robin key theo thu tu giong `tools/gemini_filter`.
Co the dung 1 trong 3 cach:

```powershell
# C1: key danh so
setx GEMINI_API_KEY_1 "key_1"
setx GEMINI_API_KEY_2 "key_2"

# C2: chuoi key
setx GEMINI_API_KEYS "key_1,key_2,key_3"

# C3: 1 key don (fallback)
setx GEMINI_API_KEY "key_1"
```

## AWS config

Tao file:

`docs/library/small_pdf_helpers/aws/aws_config.json`

Theo mau truoc do trong module goc.
