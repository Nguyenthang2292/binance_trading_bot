# Agent Runbook: Internal Document Library

**Version:** 1.0
**Date:** 2026-05-18
**Audience:** AI agents (automated pipeline)
**Design doc:** `docs/design/2026-05-18-internal-library-index-v1.0.md`

---

## Quick Reference

| File | Vai trò |
|---|---|
| `docs/library/index.json` | Machine-readable index — ĐỌC từ đây, KHÔNG edit tay |
| `docs/library/schema.json` | JSON Schema để validate index.json |
| `docs/strategies/**/00-book-references.md` | Source of truth — VIẾT vào đây |
| `tools/build_library_index.py` | Generator — CHẠY sau mỗi thay đổi |

---

## Checklist Nhanh

- [ ] Lookup sách → đọc `index.json`, filter theo `book_id`
- [ ] Tạo strategy mới → thêm dòng `**Source ref:**` + cập nhật `00-book-references.md` + chạy generator
- [ ] Thêm sách mới → tạo `00-book-references.md` mới + chạy generator
- [ ] Validate → chạy generator, kiểm tra exit code = 0

---

## 1. Tra Cứu Nguồn Của Một Strategy

**Khi nào dùng:** Agent cần biết strategy X đến từ tài liệu nào.

```python
import json

with open("docs/library/index.json") as f:
    library = json.load(f)

# Tìm theo book_id
entry = next((e for e in library["entries"] if e["book_id"] == "penfold-2020-universal-tactics"), None)

# Tìm theo strategy file path
strategy_file = "docs/strategies/golden_crossover.md"
entry = next(
    (e for e in library["entries"]
     if any(impl["file"] == strategy_file for impl in e["implemented_in"])),
    None
)

if entry:
    print(f"Title: {entry['title']}")
    print(f"Authors: {', '.join(entry['authors'])}")
    print(f"Year: {entry['year']}")
    print(f"Buy: {entry['purchase_url']}")
```

---

## 2. Tạo Strategy Mới Từ Sách Đã Có Trong Library

**Khi nào dùng:** Agent tạo strategy mới, sách đã có trong `index.json`.

**Bước 1 — Xác nhận sách tồn tại:**
```python
import json

with open("docs/library/index.json") as f:
    library = json.load(f)

book_id = "penfold-2020-universal-tactics"
entry = next((e for e in library["entries"] if e["book_id"] == book_id), None)
assert entry is not None, f"book_id '{book_id}' not found in library"
```

**Bước 2 — Tạo strategy file, thêm dòng `Source ref` vào header:**
```markdown
# <Strategy Name>

**Type:** `<type>`
**Version:** 1.0
**Date:** 2026-05-18
**Source ref:** `penfold-2020-universal-tactics` — Chapter 7, "<Chapter Name>"
**Design doc:** [...]
```

Contract machine-readable:

```regex
^\*\*Source ref:\*\*\s+`(?P<book_id>[a-z0-9-]+)`(?:\s+—\s+(?P<note>.+))?\s*$
```

Nếu có nhiều nguồn, thêm nhiều dòng `**Source ref:**`. Generator sẽ báo lỗi nếu `book_id` không tồn tại hoặc file strategy chưa được link ngược trong `implemented_chapters`.

**Bước 3 — Cập nhật `implemented_chapters` trong `00-book-references.md`:**

Mở file `docs/strategies/universal-tactics-trend-trading/00-book-references.md`, thêm entry vào YAML frontmatter:
```yaml
implemented_chapters:
  # ... entries hiện có ...
  - chapter: "Chapter 7 — <New Strategy>"
    file: "docs/strategies/<new_strategy>.md"
```

`file` phải là canonical strategy file có dòng `Source ref` tương ứng. Không dùng `pass-*.md` hoặc chapter note làm target của `implemented_chapters`.

**Bước 4 — Regenerate index:**
```bash
python tools/build_library_index.py
# Phải thấy exit code 0
# Phải thấy: [OK] Written: docs/library/index.json
```

---

## 3. Thêm Sách Mới Vào Library

**Khi nào dùng:** Agent cần tạo strategy từ một cuốn sách chưa có trong library.

**Bước 1 — Tạo folder và file source:**
```bash
mkdir docs/strategies/<book-slug>/
# Ví dụ: docs/strategies/elder-2002-come-into-my-trading-room/
```

**Bước 2 — Tạo `00-book-references.md` với YAML frontmatter:**

```markdown
---
book_id: "<author>-<year>-<short-title>"
type: "book"
title: "<Full Title>"
authors:
  - "<Author Full Name>"
year: <YYYY>
publisher: "<Publisher>"
identifiers:
  isbn_hardback: "<13-digit ISBN hoặc null>"
  isbn_pdf: null
  isbn_epub: null
  lccn: null
  doi: null
  url: null
purchase_url: "https://www.amazon.com/dp/<ASIN>"
implemented_chapters: []
---

# <Full Title>

**Author:** <Author>
**Publication:** <Publisher>, <Year>

<!-- Ghi chú thêm về cuốn sách -->
```

**Quy tắc `book_id`:**
- Format: `<lastname>-<year>-<slug>` — lowercase, dùng `-`
- Ví dụ: `elder-2002-come-into-my-trading-room`
- Paper: `fama-1970-efficient-market`
- Online: `investopedia-2024-golden-cross`

**Bước 3 — Regenerate index:**
```bash
python tools/build_library_index.py
```

---

## 4. Thêm Research Paper

Giống sách, nhưng dùng `doi` thay `isbn`:

```yaml
---
book_id: "fama-1970-efficient-market"
type: "paper"
title: "Efficient Capital Markets: A Review of Theory and Empirical Work"
authors:
  - "Eugene F. Fama"
year: 1970
publisher: "Journal of Finance"
identifiers:
  isbn_hardback: null
  doi: "10.2307/2325486"
  url: null
purchase_url: "https://doi.org/10.2307/2325486"
implemented_chapters: []
---
```

---

## 5. Thêm Online Source

Dùng `url` làm identifier chính:

```yaml
---
book_id: "investopedia-2024-golden-cross"
type: "online"
title: "Golden Cross: What It Is, With Example and What It Signals"
authors:
  - "James Chen"
year: 2024
publisher: null
identifiers:
  url: "https://www.investopedia.com/terms/g/goldencross.asp"
purchase_url: "https://www.investopedia.com/terms/g/goldencross.asp"
implemented_chapters: []
---
```

---

## 6. Validate Library (Kiểm Tra Tính Nhất Quán)

**Chạy generator và kiểm tra:**
```bash
python tools/build_library_index.py
echo "Exit code: $?"
```

**Cross-reference do generator kiểm tra:**
- Mỗi strategy file có `**Source ref:**` → `book_id` phải tồn tại trong `index.json`
- Mỗi strategy file có `**Source ref:**` → phải có link ngược trong `implemented_chapters`
- Mỗi entry trong `implemented_in[]` → nếu file tồn tại, file phải có `Source ref` tương ứng
- Mỗi entry trong `implemented_in[]` → nếu file chưa tồn tại, generator chỉ warn

**Tìm sách chưa được triển khai:**
```python
import json

with open("docs/library/index.json") as f:
    library = json.load(f)

not_implemented = [
    e for e in library["entries"]
    if len(e.get("implemented_in", [])) == 0
]

for e in not_implemented:
    print(f"Not implemented: {e['book_id']} — {e['title']}")
```

---

## 7. Schema Các Field Bắt Buộc / Tùy Chọn

| Field trong `00-book-references.md` | Bắt buộc | Ghi chú |
|---|---|---|
| `book_id` | Có | Unique toàn project |
| `type` | Có | `book` / `paper` / `online` |
| `title` | Có | |
| `authors` | Có | Mảng, ít nhất 1 phần tử |
| `year` | Có | Số nguyên |
| `publisher` | Không | `null` nếu online source |
| `identifiers.isbn_*` | Không | Dùng cho sách |
| `identifiers.doi` | Không | Dùng cho paper |
| `identifiers.url` | Không | Dùng cho online source |
| `purchase_url` | Không | `null` nếu không có |
| `implemented_chapters` | Không | Mảng rỗng `[]` nếu chưa có |

---

## 8. Lỗi Thường Gặp

| Lỗi | Nguyên nhân | Cách fix |
|---|---|---|
| `missing required field 'book_id'` | `00-book-references.md` thiếu YAML frontmatter hoặc thiếu field | Thêm field bắt buộc vào frontmatter |
| `duplicate book_id: penfold-2020-...` | Hai file cùng `book_id` | Đổi tên `book_id` của file mới tạo |
| `[WARN] file not found: docs/strategies/...` | `implemented_chapters[].file` trỏ sai path | Kiểm tra đường dẫn chính xác |
| `unknown source ref: <book_id>` | Strategy file trỏ tới `book_id` không tồn tại | Thêm/correct `00-book-references.md` hoặc sửa `Source ref` |
| `source ref missing from implemented_chapters` | Strategy có `Source ref` nhưng source file chưa link ngược | Thêm strategy vào `implemented_chapters` |
| `implemented file missing source ref` | `implemented_chapters[].file` tồn tại nhưng thiếu `Source ref` tương ứng | Thêm dòng `**Source ref:**` vào strategy |
| Generator không tìm thấy file nào | Chạy từ sai thư mục | Chạy từ project root |
