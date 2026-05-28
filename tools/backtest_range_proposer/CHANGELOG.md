# Changelog — tools/backtest_range_proposer

All notable changes to the backtest range proposer CLI are documented here.

Format inspired by [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## 2026-05-26 — `main.py`

Driven by code review of `main.py`. All structural changes are covered by tests
in `tests/python/test_backtest_range_proposer.py`. The stdin/stdout/exit-code
contract documented at the top of the file is unchanged.

### Fixed — Critical

- **Output validation enforces every rule the prompt advertises to Gemini.**
  `_validate_output_ranges` now also checks (a) `is_integer=True` params have
  whole-number `min`/`max`/`step`, (b) `is_integer` is unchanged versus
  `default_ranges`, (c) every range covers the corresponding `current_values`
  entry, and (d) `constraints` of kind `less_than` / `less_than_or_equal` are
  feasible (`left.min < right.max` or `<=`). Previously these were on the
  honour system and a non-compliant Gemini response would pass through.
- **Per-attempt timeout × `key_count` could exceed the outer wall-clock budget
  when the 1 000 ms minimum-clamp dominated** (e.g. 6 000 ms total ÷ 12 keys
  produced 1 000 ms × 12 = 12 s, killed by the C++ caller). Removed the
  minimum clamp at per-attempt level; emit a `WARNING` log instead when the
  split falls below the recommended floor, prioritising "do not exceed total
  budget" over "give each attempt a usable timeout."

### Fixed — Input validation & defense-in-depth

- **Type checks on identity fields and required objects.** `_validate_input`
  now enforces `eval_id` / `symbol` / `interval` / `strategy_id` are non-empty
  strings, `current_values` and `prompt_context_aggregates` are objects,
  `tunable_params` contains only non-empty strings, and `default_ranges` is a
  list of well-formed range entries.
- **Prompt-injection hardening.** New `_validate_prompt_identity_field` rejects
  newlines (`\n` / `\r`) and markdown markers (`#`) in `symbol`, `interval`,
  and `strategy_id` so a crafted input cannot inject additional prompt sections.
- **Strict `budget.timeout_seconds` parsing.** New
  `_derive_outer_timeout_seconds` rejects floats with a fractional part, `bool`,
  non-digit strings, and non-positive values. `timeout_seconds=7.9` now returns
  a structured `invalid_input` error instead of silently truncating to `7`.
- **Fail-fast on unusable outer budget.** When `outer_timeout_seconds <=
  GEMINI_TIMEOUT_HEADROOM_SECONDS` (≤ 2 s), the CLI returns `invalid_input`
  immediately rather than attempting a Gemini call that will certainly be
  killed.

### Changed — Reliability & maintainability

- **Atomic output write.** `_write_output` now writes to a sibling `.tmp` file
  and `replace()`s the target, eliminating partial-file reads from the C++
  consumer if the process is killed mid-write.
- **`logging.basicConfig` moved out of `main()`.** A new
  `_configure_logging_once` is called from the `__name__ == "__main__"` guard
  only, so repeated `main()` invocations from tests do not leak logging state.
- **Deduplicated error-emission boilerplate.** Three nearly-identical
  `except Exception: LOGGER.exception(...); _error_result(...); _write_output(...)`
  blocks collapsed into `_emit_error_and_return`. The input-parse and
  response-parse paths now catch only the narrow set
  `(OSError, json.JSONDecodeError, TypeError, ValueError)`; the Gemini call
  still catches `Exception` because the SDK raises a wide range of types.
- **Readability in `_call_gemini`.** The two-instance `GeminiKeyManager`
  construction now uses an explicit `if key_count <= 1` branch instead of a
  ternary keyed on a coincidental equality between `per_attempt_timeout_ms` and
  `total_budget_ms`.
