---
description: Full multi-dimensional review of the entire src/ C++ tree; writes a dated report to docs/comprehensive-reviews/
argument-hint: (no args — always reviews the whole src/ tree)
---

# /src-review — Whole-`src/` C++ Review

You are performing a **comprehensive, multi-dimensional code review of the ENTIRE
`src/` C++ tree** of this Binance trading bot.

- Any argument passed to this command is **ignored** — always review all of `src/`.
- The deliverable is **one dated Markdown report** written to
  `docs/comprehensive-reviews/`, matching the existing report convention.
- This command is **read-only**: do NOT modify, fix, or refactor any code.
- Do NOT print findings in chat — only a short pointer at the very end
  (this project's convention for this command is "saved report only").

Work through the steps below in order.

---

## Step 1 — Determine version & prior review

1. Resolve today's date as `YYYY-MM-DD`.
2. Search both `docs/comprehensive-reviews/*.md` and
   `docs/comprehensive-reviews/archives/*.md` for files matching `*-src-v*.md`
   (i.e. previous whole-tree reviews).
   - **None found** → this review is **`v1.0`**; there is no prior review.
   - **Found** → take the highest existing version `N.M`; this review is the
     next **minor** version `N.(M+1)`. Record the path of the latest prior
     report as the **Prior review**, and carry forward its `CR-#`/`WR-#`/`IN-#`
     finding IDs so you can mark each as ✅ Resolved / ⏭ Deferred / 🔁 Regressed
     / unchanged — exactly as the existing per-module reviews track findings
     across versions.
3. Open **one recent** report in `docs/comprehensive-reviews/` (e.g. the most
   recent `*.md`) and skim it so your output matches its tone and formatting.

## Step 2 — Enumerate modules

List the immediate subdirectories of `src/` **plus** any root-level
`src/*.{h,cpp}` files. Treat each as one review unit. Discover these live — do
not assume the list. (At time of writing they are: `orders/`, `engine/`,
`risk/`, `ws/`, `rest/`, `transport/`, `strategy/`, `orchestration/`,
`scanner/`, `catalog/`, `types/`, and root files such as `context.{h,cpp}` and
`logger.{h,cpp}`.)

If `src/` is missing or has no `.h`/`.cpp` files, **abort** with a clear message
and write nothing.

## Step 3 — Dispatch parallel review subagents (one per module)

Dispatch one `general-purpose` subagent **per module, in parallel** (issue the
Task calls together in a single message). Give each subagent this brief, with
`<module>` filled in:

> Review **every** `.h`/`.cpp` file under `src/<module>/` for a production
> Binance trading bot. Cover all four dimensions:
>
> 1. **Correctness & bugs** — logic errors, edge cases, error/exception
>    handling, API misuse, off-by-one, null/empty/uninitialized values,
>    resource leaks.
> 2. **Concurrency & memory safety** — data races, lock discipline (especially
>    the scheduler/WS threads and work queues), deadlock, lifetime/ownership,
>    dangling references, RAII gaps, undefined behavior.
> 3. **Security & secrets** — API key/secret handling, HMAC request-signing
>    correctness, `.env`/credential leakage into logs, injection, unsafe
>    input/JSON parsing.
> 4. **Financial & performance** — decimal precision/rounding, order sizing &
>    risk math correctness, and latency/allocation issues on hot paths.
>
> Return a structured list of findings. For each finding give: a one-line
> title, a severity (`Critical` / `Warning` / `Info`), the `file:line`, a
> concise explanation of the risk, and a recommended fix. Be precise with file
> paths and line numbers. **Do not modify any code.** If the module looks
> clean on a dimension, say so explicitly.

If a subagent fails or returns nothing usable, note that module as a coverage
gap in the report's Overview rather than dropping it silently.

## Step 4 — Synthesize

1. Collect all subagent findings and **deduplicate** cross-module overlaps.
2. Assign stable global IDs by severity: `CR-#` (🔴 Critical), `WR-#`
   (🟡 Warning), `IN-#` (⚪ Info).
3. If a prior `*-src-v*.md` review exists, reconcile its findings (mark each
   ✅ Resolved / ⏭ Deferred / 🔁 Regressed) and add the new ones.
4. Form an overall **verdict** (Approve / Approve with fixes / Needs work).

## Step 5 — Write the report

Write to `docs/comprehensive-reviews/<YYYY-MM-DD>-src-v<N.M>.md` using this
structure (match the existing reports' style; use fenced code blocks for any
code snippets):

````markdown
# Comprehensive Review: `src/` Tree (Full)

**Date:** <YYYY-MM-DD>
**Status:** <✅ COMPLETE — approve | ⚠️ ISSUES FOUND | ❌ NEEDS WORK>
**Reviewer:** Claude Code (multi-dimensional review, parallel module subagents)
**Scope:** Entire `src/` C++ tree — modules: <comma-separated module list>. <note on what changed vs prior review, if any>
**Module:** Full source tree
**Prior review:** <relative link to latest prior *-src-v*.md, or "none — first full-tree review">

---

## Overview

<2–4 sentence summary of the tree's health and the headline findings. Note any module coverage gaps here.>

| Dimension | Assessment |
|---|---|
| Code quality | … |
| Architecture | … |
| Security / data integrity | … |
| Performance | … |
| Concurrency | … |
| Testing | … |

---

## Findings

### 🔴 Critical

#### CR-1 — <title>
**File:** [`src/.../file.cpp:NN`](../../src/.../file.cpp)
<explanation of the risk>
**Recommendation:** <concrete fix>

<…repeat for each critical finding…>

### 🟡 Warning

#### WR-1 — <title>
**File:** [`src/.../file.cpp:NN`](../../src/.../file.cpp)
<explanation>
**Recommendation:** <fix>

<…repeat…>

### ⚪ Info

#### IN-1 — <title>
**File:** [`src/.../file.cpp:NN`](../../src/.../file.cpp)
<explanation>
**Recommendation:** <fix>

<…repeat…>

<If a prior review exists, add the following section:>

---

## Prior Findings — Verification

| ID | Issue | This version |
|---|---|---|
| CR-1 | <prior title> | ✅ Resolved / ⏭ Deferred / 🔁 Regressed |
<…one row per prior finding…>

---

## Verdict

<Approve / Approve with fixes / Needs work> — <one-paragraph reasoning>.

---

## Severity Summary

| ID | Title | Severity | Status |
|---|---|---|---|
| CR-1 | … | 🔴 Critical | New / Carried / Resolved |
| WR-1 | … | 🟡 Warning | … |
| IN-1 | … | ⚪ Info | … |
````

**Path note:** the report lives in `docs/comprehensive-reviews/`, so file links
are relative as `../../src/...` (two levels up to the repo root). Verify links
resolve.

## Step 6 — Report back in chat

Print **only**:

- the report path,
- severity counts (`🔴 N critical · 🟡 N warning · ⚪ N info`),
- the one-line verdict.

Do not print the findings inline.

---

## Hard rules

- **Read-only.** Never edit, fix, or refactor `src/` (or any other) code.
- Write **exactly one** report file. Do **not** auto-archive older reports —
  moving superseded reports into `archives/` is a manual step.
- Do not review the Python/qlib side of the project; `@src` is the C++ tree only.
