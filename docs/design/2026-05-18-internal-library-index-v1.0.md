# Internal Document Library & Agent Retrieval Runbook — v1.0

**Date:** 2026-05-18
**Status:** DONE
**Audience:** AI agents, human developers

**Tài liệu liên quan:**
- `docs/runbooks/strategy-design-runbook.md` — quy trình tạo strategy mới
- `docs/strategies/universal-tactics-trend-trading/00-book-references.md` — ví dụ source file hiện có
- `docs/library/runbook.md` — runbook cho agents truy xuất library

---

## 1. Mục Tiêu

Project đang mở rộng chiến lược từ nhiều nguồn tài liệu (sách, paper, nguồn online). Không có cơ chế tập trung nào để:

1. Biết strategy X được triển khai từ cuốn sách nào
2. Biết cuốn sách Y đã được triển khai thành những strategy nào
3. Tìm link mua / metadata đầy đủ của nguồn tài liệu

Design này xây dựng một **internal document library** với ba thành phần:

| Thành phần | Mục đích |
|---|---|
| `docs/library/index.json` | Machine-readable index, auto-generated, không edit tay |
| `docs/library/schema.json` | JSON Schema validate output của generator |
| `docs/library/runbook.md` | Hướng dẫn agents truy xuất và cập nhật library |

---

## 2. Nguyên Tắc Thiết Kế

- **Single source of truth:** Metadata tài liệu nằm trong các file `00-book-references.md` — con người viết và đọc được. `index.json` chỉ là derivative.
- **Bidirectional linking:** Library có `implemented_in[]` trỏ tới canonical strategy files; strategy files có dòng `**Source ref:**` trỏ về `book_id`.
- **Auto-generated index:** Script `tools/build_library_index.py` quét `**/00-book-references.md` và emit `index.json`. Agent chạy script này sau mỗi lần tạo strategy mới.
- **Extensible schema:** Hỗ trợ sách (ISBN), research paper (DOI), và nguồn online (URL fallback).

---

## 3. Cấu Trúc File

```
docs/
  library/
    index.json          ← AUTO-GENERATED — không edit tay
    schema.json         ← JSON Schema để validate index.json
    runbook.md          ← hướng dẫn agents
  strategies/
    universal-tactics-trend-trading/
      00-book-references.md   ← SOURCE OF TRUTH (YAML frontmatter + Markdown body)
      01-random-trend-trader.md
      03-gartley-3-and-6-week-crossover.md
      ...
    golden_crossover.md         ← có dòng Source ref
    monthly_close_model.md      ← có dòng Source ref
    donchian_5_20_crossover.md
    gartley_day_crossover.md
    trend_breakout.md
tools/
  build_library_index.py    ← generator script
```

---

## 4. Format `00-book-references.md` (Source of Truth)

Mỗi source folder phải có file `00-book-references.md` với **YAML frontmatter** ở đầu file, theo sau là Markdown body tự do.

### 4.1 YAML Frontmatter Schema

```yaml
---
book_id: "penfold-2020-universal-tactics"      # slug duy nhất: <author>-<year>-<short-title>
type: "book"                                    # book | paper | online
title: "The Universal Tactics of Successful Trend Trading"
authors:
  - "Brent Norman Lindsay Penfold"
year: 2020
publisher: "Wiley"                              # bỏ qua nếu type=online
identifiers:
  isbn_hardback: "9781119734512"
  isbn_pdf: "9781119734550"
  isbn_epub: "9781119734499"
  lccn: "2020020364"
  doi: null                                     # dùng cho paper
  url: null                                     # dùng cho online source
purchase_url: "https://www.amazon.com/dp/1119734517"
implemented_chapters:                           # các chapter/phần đã được triển khai
  - chapter: "Chapter 6 — Random Trend Trader"
    file: "docs/strategies/trend_breakout.md"
  - chapter: "Chapter 7 — Gartley"
    file: "docs/strategies/gartley_day_crossover.md"
  - chapter: "Chapter 7 — Donchian 5/20"
    file: "docs/strategies/donchian_5_20_crossover.md"
  - chapter: "Chapter 7 — Golden 50/200"
    file: "docs/strategies/golden_crossover.md"
  - chapter: "Chapter 7 — Monthly Close"
    file: "docs/strategies/monthly_close_model.md"
---
```

### 4.2 Quy tắc `book_id`

Format: `<lastname>-<year>-<slug>` — tất cả lowercase, dùng `-` thay space.

| Loại | Ví dụ |
|---|---|
| Sách | `penfold-2020-universal-tactics` |
| Paper | `fama-1970-efficient-market` |
| Online | `investopedia-2024-golden-cross` |

### 4.3 Ví dụ cho Research Paper

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
  doi: "10.2307/2325486"
  isbn_hardback: null
purchase_url: "https://doi.org/10.2307/2325486"
implemented_chapters: []
---
```

### 4.4 Ví dụ cho Online Source

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

## 5. Format Strategy File — Dòng `Source ref`

Mỗi canonical strategy file trong `docs/strategies/` thêm dòng `**Source ref:**` vào phần header:

```markdown
# Golden 50/200 Moving Average Crossover (Crypto MTF State Variant)

**Type:** `golden_crossover`
**Version:** 1.0
**Date:** 2026-05-16
**Source ref:** `penfold-2020-universal-tactics` — Chapter 7, "Golden 50 and 200-Day Crossover"
**Design doc:** [...]
```

Quy tắc:
- Giá trị là `book_id` (khớp với `book_id` trong `00-book-references.md`)
- Nếu có nhiều nguồn: liệt kê nhiều dòng `**Source ref:**`
- Nếu không có nguồn tài liệu: bỏ qua field này (không bắt buộc)
- Cú pháp machine-readable duy nhất:

```regex
^\*\*Source ref:\*\*\s+`(?P<book_id>[a-z0-9-]+)`(?:\s+—\s+(?P<note>.+))?\s*$
```

Generator scan các file `docs/strategies/**/*.md`, bỏ qua `**/00-book-references.md` và `**/pass-*.md`. Nếu một file có `Source ref`, mỗi `book_id` phải tồn tại trong library và phải được phản ánh trong đúng `implemented_chapters[].file` của source tương ứng.

`implemented_chapters[].file` phải trỏ tới canonical strategy file có dòng `Source ref` tương ứng. Các chapter note / pass file chỉ là working notes, không đưa vào `implemented_chapters` trừ khi chúng chính là strategy spec canonical.

---

## 6. `docs/library/index.json` — Output Format

File này được **auto-generated** bởi `tools/build_library_index.py`. Không chỉnh sửa tay.

```json
{
  "generator": "tools/build_library_index.py",
  "version": "1.0",
  "entries": [
    {
      "book_id": "penfold-2020-universal-tactics",
      "type": "book",
      "title": "The Universal Tactics of Successful Trend Trading",
      "authors": ["Brent Norman Lindsay Penfold"],
      "year": 2020,
      "publisher": "Wiley",
      "identifiers": {
        "isbn_hardback": "9781119734512",
        "isbn_pdf": "9781119734550",
        "isbn_epub": "9781119734499",
        "lccn": "2020020364",
        "doi": null,
        "url": null
      },
      "purchase_url": "https://www.amazon.com/dp/1119734517",
      "implemented_in": [
        {
          "chapter": "Chapter 7 — Golden 50/200",
          "file": "docs/strategies/golden_crossover.md"
        }
      ],
      "source_file": "docs/strategies/universal-tactics-trend-trading/00-book-references.md"
    }
  ]
}
```

Output phải deterministic:
- Sort `entries` theo `book_id`
- Sort `implemented_in` theo `file`, rồi `chapter`
- Không ghi timestamp động vào `index.json` để tránh diff churn khi chỉ chạy validation
- Chỉ rewrite `docs/library/index.json` nếu nội dung semantic thay đổi

---

## 7. Generator Script — `tools/build_library_index.py`

### 7.1 Trách nhiệm

1. Quét toàn bộ `docs/**/00-book-references.md`
2. Parse YAML frontmatter từ mỗi file
3. Scan `docs/strategies/**/*.md` để validate các dòng `**Source ref:**`
4. Validate schema và cross-reference (báo lỗi rõ ràng nếu thiếu field bắt buộc)
5. Emit `docs/library/index.json`

### 7.2 Interface

```bash
# Chạy từ project root
python tools/build_library_index.py

# Output
# [OK]  Processed: docs/strategies/universal-tactics-trend-trading/00-book-references.md
# [OK]  Written: docs/library/index.json (1 entries)

# Nếu có lỗi schema
# [ERR] docs/strategies/some-book/00-book-references.md: missing required field 'book_id'
# Exit code 1
```

### 7.3 Logic xử lý lỗi

| Lỗi | Hành vi |
|---|---|
| File không có YAML frontmatter | Skip + warn, không crash |
| Thiếu field bắt buộc (`book_id`, `type`, `title`, `authors`, `year`) | Báo lỗi + exit code 1 |
| `type` không thuộc `book`, `paper`, `online` | Báo lỗi + exit code 1 |
| `book_id` không match `^[a-z0-9-]+$` | Báo lỗi + exit code 1 |
| `book_id` trùng lặp | Báo lỗi + exit code 1 |
| `implemented_chapters[].file` không tồn tại | Warn (không exit) — file có thể chưa được tạo |
| Strategy có `Source ref` trỏ tới `book_id` không tồn tại | Báo lỗi + exit code 1 |
| Strategy có `Source ref` nhưng không có trong `implemented_chapters` của source tương ứng | Báo lỗi + exit code 1 |
| `implemented_chapters[].file` tồn tại nhưng thiếu `Source ref` tương ứng | Báo lỗi + exit code 1 |

### 7.4 Fields bắt buộc vs tùy chọn

| Field | Bắt buộc | Mặc định nếu thiếu |
|---|---|---|
| `book_id` | Có | — |
| `type` | Có | — |
| `title` | Có | — |
| `authors` | Có | — |
| `year` | Có | — |
| `publisher` | Không | `null` |
| `identifiers.*` | Không | `null` |
| `purchase_url` | Không | `null` |
| `implemented_chapters` | Không | `[]` |

---

## 8. `docs/library/schema.json` — JSON Schema

```json
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "LibraryIndex",
  "type": "object",
  "required": ["generator", "version", "entries"],
  "properties": {
    "generator": { "type": "string" },
    "version": { "type": "string" },
    "entries": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["book_id", "type", "title", "authors", "year", "implemented_in", "source_file"],
        "properties": {
          "book_id": { "type": "string", "pattern": "^[a-z0-9-]+$" },
          "type": { "type": "string", "enum": ["book", "paper", "online"] },
          "title": { "type": "string" },
          "authors": { "type": "array", "items": { "type": "string" }, "minItems": 1 },
          "year": { "type": "integer", "minimum": 1900, "maximum": 2100 },
          "publisher": { "type": ["string", "null"] },
          "identifiers": {
            "type": "object",
            "properties": {
              "isbn_hardback": { "type": ["string", "null"] },
              "isbn_pdf": { "type": ["string", "null"] },
              "isbn_epub": { "type": ["string", "null"] },
              "lccn": { "type": ["string", "null"] },
              "doi": { "type": ["string", "null"] },
              "url": { "type": ["string", "null"] }
            },
            "additionalProperties": false
          },
          "purchase_url": { "type": ["string", "null"] },
          "implemented_in": {
            "type": "array",
            "items": {
              "type": "object",
              "required": ["file"],
              "properties": {
                "chapter": { "type": "string" },
                "file": { "type": "string" }
              },
              "additionalProperties": false
            }
          },
          "source_file": { "type": "string" }
        },
        "additionalProperties": false
      }
    }
  },
  "additionalProperties": false
}
```

---

## 9. Runbook Agent — `docs/library/runbook.md`

> Nội dung chi tiết của runbook được đặt tại `docs/library/runbook.md`. Design doc này mô tả các ca sử dụng mà runbook phải xử lý.

### Ca 1: Tra cứu nguồn của một strategy

**Input:** Agent có `book_id` hoặc tên strategy
**Quy trình:**
1. Đọc `docs/library/index.json`
2. Tìm entry có `book_id` khớp (hoặc filter `implemented_in[].file` khớp tên strategy)
3. Trả về: `title`, `authors`, `year`, `purchase_url`, và danh sách `implemented_in`

### Ca 2: Tạo strategy mới từ một cuốn sách

**Quy trình:**
1. Kiểm tra `docs/library/index.json` — sách đã có chưa?
2. Nếu chưa: tạo `00-book-references.md` mới với YAML frontmatter đầy đủ
3. Tạo strategy file với dòng `**Source ref:**` trỏ về `book_id`
4. Cập nhật `implemented_chapters` trong `00-book-references.md`
5. Chạy `python tools/build_library_index.py` để regenerate index
6. Kiểm tra exit code — phải là 0

### Ca 3: Kiểm tra tính nhất quán (validation)

**Quy trình:**
1. Chạy `python tools/build_library_index.py`
2. Nếu exit code != 0: đọc stderr, fix lỗi
3. Generator tự cross-check: với mỗi strategy file có `Source ref`, `book_id` tương ứng phải tồn tại trong `index.json` và phải có link ngược trong `implemented_chapters`

### Ca 4: Tìm sách chưa được triển khai đầy đủ

**Quy trình:**
1. Đọc `docs/library/index.json`
2. Filter các entry có `implemented_in` rỗng hoặc ít hơn số chapter trong sách
3. Cross-check với các file `pass-*.md` trong strategy folders (đây là các chapter đã đọc nhưng chưa triển khai)

---

## 10. Quy Trình Cập Nhật Khi Thêm Sách Mới

```
1. Tạo folder mới: docs/strategies/<book-slug>/
2. Tạo file:       docs/strategies/<book-slug>/00-book-references.md
   └── YAML frontmatter đầy đủ (xem Section 4)
   └── Markdown body: thông tin xuất bản, ghi chú cá nhân
3. Chạy:           python tools/build_library_index.py
4. Kiểm tra:       docs/library/index.json đã có entry mới
```

---

## 11. Quy Trình Cập Nhật Khi Thêm Strategy Từ Sách Đã Có

```
1. Tạo strategy file tại docs/strategies/<name>.md
   └── Thêm dòng: **Source ref:** `<book_id>` — <chapter name>
2. Cập nhật 00-book-references.md của sách đó:
   └── Thêm entry vào implemented_chapters[]
3. Chạy:  python tools/build_library_index.py
4. Kiểm tra exit code = 0
```

---

## 12. Decision Log

| # | Quyết định | Lựa chọn thay thế | Lý do chọn |
|---|---|---|---|
| D1 | Source of truth là `00-book-references.md` với YAML frontmatter, không phải `index.json` | Viết tay `index.json` trực tiếp | Con người vẫn đọc được; tránh drift; phù hợp cấu trúc folder đã có |
| D2 | `index.json` là derivative, auto-generated | Maintain song song hai nguồn | Loại bỏ hoàn toàn khả năng drift; agent không cần quyết định cái nào đúng |
| D3 | JSON thay vì YAML cho output | YAML | Parse dễ hơn trong cả Python lẫn C++ không cần thư viện nặng |
| D4 | `book_id` dạng slug `<author>-<year>-<slug>` | UUID, ISBN làm key | Dễ đọc, dễ nhớ, human-meaningful trong strategy file |
| D5 | Bidirectional link: library → strategy VÀ strategy → library | Chỉ một chiều | Agent có thể navigate từ cả hai phía; phát hiện inconsistency dễ hơn |
| D6 | 1 link mua duy nhất (Amazon ưu tiên) | Multi-region links | Đơn giản, maintain dễ; scope hiện tại chưa cần multi-region |
| D7 | Script Python, dùng parser frontmatter/YAML tối thiểu trong stdlib | mkdocs, Jekyll, PyYAML bắt buộc | Không thêm dependency nặng; schema hiện tại chỉ dùng YAML subset đơn giản đủ parse bằng stdlib |
| D8 | Agent tự động chạy script sau khi tạo strategy | Human approve trước | Giảm friction; script chỉ read + write file local, không có side effect nguy hiểm |
| D9 | Không ghi timestamp động vào `index.json` | `generated_at` bắt buộc | Tránh diff churn khi agent chỉ chạy validation |

---

## 13. Assumptions

| # | Assumption |
|---|---|
| A1 | `tools/build_library_index.py` chạy từ project root với Python 3.8+ |
| A2 | YAML frontmatter trong `00-book-references.md` được delimited bởi `---` ở đầu và cuối |
| A3 | Frontmatter chỉ dùng YAML subset đang mô tả trong Section 4: scalar string/int/null, list string, list object một cấp, object một cấp |
| A4 | `book_id` là globally unique trong toàn bộ project |
| A5 | Các file `pass-*.md` (pass-02-hearne-1-percent-rule.md) là chapter đã đọc nhưng không triển khai — không nằm trong `implemented_chapters` |
| A6 | `docs/library/index.json` được commit vào git cùng với source changes |

---

## 14. Implementation Plan (Thứ tự thực hiện)

1. **Cập nhật `00-book-references.md` hiện có** — thêm YAML frontmatter vào file `docs/strategies/universal-tactics-trend-trading/00-book-references.md`
2. **Cập nhật strategy files hiện có** — thêm dòng `**Source ref:**` vào `golden_crossover.md`, `monthly_close_model.md`, `donchian_5_20_crossover.md`, `gartley_day_crossover.md`, `trend_breakout.md`
3. **Tạo `tools/build_library_index.py`**
4. **Tạo `docs/library/schema.json`**
5. **Chạy generator** — verify `docs/library/index.json` được tạo đúng
6. **Tạo `docs/library/runbook.md`** — copy nội dung từ Section 9 + expand
