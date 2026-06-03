# Design: `/src-review` Slash Command (whole-`src/` C++ review)

**Date:** 2026-05-29
**Status:** Approved (pending spec review)
**Author:** Claude Code (brainstorming session)

## Purpose

A custom Claude Code slash command, `/src-review`, that performs a
comprehensive, multi-dimensional review of the **entire `src/` C++ tree** of
this Binance trading bot and writes a dated Markdown report that matches the
project's existing `docs/comprehensive-reviews/` convention.

The command is read-only: it never edits, fixes, or refactors code. Its only
side effect is writing one report file.

## Decisions (locked via brainstorming)

| Question | Decision |
|---|---|
| Artifact type | Slash command at `.claude/commands/src-review.md`, invoked as `/src-review`. |
| Scope | Always sweep the **whole `src/` tree**. Any argument is ignored. |
| Review focus | All four dimensions: (1) Correctness & bugs, (2) Concurrency & memory safety, (3) Security & secrets, (4) Financial & performance. |
| Output | **Saved report only** — a dated Markdown file under `docs/comprehensive-reviews/`, with only a short pointer printed in chat. |
| Execution approach | **Approach A** — dispatch one review subagent per `src/` module in parallel, then the main agent synthesizes findings into a single report. |

## Artifact

- **File:** `.claude/commands/src-review.md` (creates the `.claude/commands/`
  directory, which does not yet exist).
- **Frontmatter:** `description` (shown in the slash-command menu) and
  `argument-hint` noting that arguments are ignored.

## Behavior

### Step 1 — Version detection & prior review

- Resolve today's date as `YYYY-MM-DD`.
- Search both `docs/comprehensive-reviews/*.md` and
  `docs/comprehensive-reviews/archives/*.md` for files matching `*-src-v*.md`.
  - **None found** → this review is `v1.0`, no prior review.
  - **Found** → take the highest existing `N.M`; the new review is the next
    minor version `N.(M+1)`. Record the latest prior report as the **Prior
    review** and carry forward its `CR/WR/IN` finding IDs so each can be marked
    ✅ Resolved / ⏭ Deferred / 🔁 Regressed / unchanged — exactly like the
    existing per-module reviews track findings across versions.

### Step 2 — Enumerate modules

List the immediate subdirectories of `src/` plus root-level `src/*.{h,cpp}`
files; treat each as one review unit. Modules are discovered live (not
hard-coded), but currently include: `orders/`, `engine/`, `risk/`, `ws/`,
`rest/`, `transport/`, `strategy/`, `orchestration/`, `scanner/`, `catalog/`,
`types/`, plus root files (`context.{h,cpp}`, `logger.{h,cpp}`).

### Step 3 — Parallel review subagents (Approach A)

Dispatch one `general-purpose` subagent per module **in parallel**. Each
subagent reviews every `.h`/`.cpp` file in its module across all four
dimensions and returns a structured finding list. Each finding includes: a
one-line title, severity (Critical / Warning / Info), `file:line`, a concise
risk explanation, and a recommended fix. Subagents must not modify code.

The four dimensions each subagent applies:

1. **Correctness & bugs** — logic errors, edge cases, error/exception handling,
   API misuse, off-by-one, null/empty/uninitialized values, resource leaks.
2. **Concurrency & memory safety** — data races, lock discipline (especially
   the scheduler/WS threads and work queues), deadlock, lifetime/ownership,
   dangling references, RAII gaps, undefined behavior.
3. **Security & secrets** — API key/secret handling, HMAC request-signing
   correctness, `.env`/credential leakage into logs, injection, unsafe
   input/JSON parsing.
4. **Financial & performance** — decimal precision/rounding, order sizing & risk
   math correctness, and latency/allocation issues on hot paths.

### Step 4 — Synthesis

The main agent collects all subagent findings, deduplicates cross-module
overlaps, and assigns stable global IDs by severity: `CR-#` (🔴 Critical),
`WR-#` (🟡 Warning), `IN-#` (⚪ Info). If a prior `*-src-v*.md` review exists,
it reconciles prior findings (Resolved / Deferred / Regressed) and adds new
ones. It then forms an overall verdict.

### Step 5 — Write the report

Write to `docs/comprehensive-reviews/<YYYY-MM-DD>-src-v<N.M>.md`, matching the
existing report format:

- **Title:** `# Comprehensive Review: \`src/\` Tree (Full)`
- **Header block:** Date, Status, Reviewer, Scope (module list + prior-review
  note), Module (`Full source tree`), Prior review (link or "none").
- **Overview** + dimension table (Code quality, Architecture, Security / data
  integrity, Performance, Concurrency, Testing).
- **Findings** grouped by severity, each with its ID, a `file:line` markdown
  link (relative `../../src/...` from the report location), explanation, and
  recommendation.
- If a prior review exists: a **Prior Findings — Verification** table.
- **Verdict.**
- **Severity Summary** table.

### Step 6 — Chat output

Print only: the report path, severity counts (🔴 N / 🟡 N / ⚪ N), and the
one-line verdict. No inline findings (per the "saved report only" decision).

## Boundaries (YAGNI)

- Read-only: never modifies, fixes, or refactors code.
- Writes exactly one report file.
- Does **not** auto-archive older reports — moving superseded reports into
  `archives/` stays a manual step, per current project practice.

## Error handling

- If `src/` is missing or contains no `.h`/`.cpp` files → abort with a clear
  message; write nothing.
- If version detection is ambiguous or fails → default to `v1.0` and note the
  assumption in the report header.
- If a module subagent fails → record the gap in the report's Overview rather
  than silently dropping that module.

## Out of scope

- Reviewing the Python/qlib side of the project (the `@src` target is the C++
  tree only).
- A CI/GitHub-Actions variant.
- Auto-fixing or opening PRs.
