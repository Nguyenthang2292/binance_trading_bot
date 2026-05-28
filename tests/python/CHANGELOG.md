# Changelog — tests/python

All notable changes to the Python test suite are documented here.

Format inspired by [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## 2026-05-26 — `test_key_manager.py`

Aligned with code-review-driven changes to `tools/shared/gemini_key_manager.py`
(see [`tools/shared/CHANGELOG.md`](../../tools/shared/CHANGELOG.md)).

### Added

- `test_generic_rate_word_is_not_retryable` — guards against the old loose
  substring matcher classifying `"sampling rate mismatch"` as key-retryable.
- `test_default_state_dir_is_absolute_and_not_cwd_relative` — pins the new
  `tempfile.gettempdir()`-anchored default.
- `test_file_lock_reclaims_stale_marker` — verifies a 120 s old lock file is
  reclaimed instead of deadlocking subsequent processes.
- `test_client_closed_after_successful_call` and
  `test_client_closed_for_each_retry_attempt` — assert `genai.Client.close()`
  is called once per attempt regardless of success or rotation.
- `test_server_error_does_not_rotate_all_keys` — fixes the contract that a
  `503` from upstream stops at the first key and surfaces immediately.
- `test_all_retryable_failures_raise_from_last_error` — ensures
  `__cause__` is set on the final `RuntimeError` when every key fails with a
  retryable error.
- `test_fallback_start_index_uses_random_when_state_claim_fails` — pins the
  random fallback when the state lock cannot be acquired.
- `test_keyset_digest_depends_on_values_not_names` — pins the new
  value-only digest so env-var renames do not reset rotation state.

### Changed

- `test_500/502/503/504_is_retryable` and `test_timeout_text_is_retryable`
  inverted to assert **not retryable** (5xx and timeouts are no longer
  classified as key-scoped failures).
- Test names renamed to match the inverted assertions
  (`..._is_retryable` → `..._is_not_retryable`).
- Fixtures and expected key-name lists updated for the
  `GEMINI_API_KEYS[1]` → `GEMINI_API_KEYS_1` rename.
- `test_retryable_error_moves_to_next_key_from_round_robin_start` updated to
  raise `"429 resource_exhausted"` (still key-retryable) instead of
  `"temporary unavailable"` (no longer key-retryable).

---

## 2026-05-26 — `test_backtest_range_proposer.py`

Aligned with code-review-driven changes to
`tools/backtest_range_proposer/main.py`
(see [`tools/backtest_range_proposer/CHANGELOG.md`](../../tools/backtest_range_proposer/CHANGELOG.md)).

### Added

- `test_validate_input_symbol_must_be_string` and
  `test_validate_input_current_values_must_be_object` — pin the new type
  checks on identity fields and required objects.
- `test_validate_input_rejects_markdown_markers_in_symbol` and
  `test_validate_input_rejects_newline_in_strategy_id` — pin the
  prompt-injection guard on identity fields.
- `test_validate_output_ranges_integer_param_requires_whole_numbers` — pin
  prompt Rule 3 (integer params have whole-number bounds).
- `test_validate_output_ranges_must_cover_current_values` — pin prompt
  Rule 5 (output ranges contain the live config).
- `test_validate_output_ranges_must_keep_default_is_integer_flag` — pin
  prompt Rule 4 (`is_integer` is immutable across the round trip).
- `test_validate_output_ranges_must_be_constraint_compatible` — pin prompt
  Rule 7 (proposed ranges admit at least one constraint-feasible pair).
- `test_main_rejects_non_integer_budget_timeout` — pin strict
  `budget.timeout_seconds` parsing (`7.9` rejected with `invalid_input`).
- `test_main_rejects_budget_timeout_not_above_headroom` — pin the fail-fast
  path when the outer budget is below the SDK headroom.
- `test_call_gemini_does_not_clamp_per_attempt_above_budget_split` — pin the
  removal of the 1 000 ms minimum-clamp (6 000 ms ÷ 12 keys = 500 ms per
  attempt, was 1 000 ms × 12 = 12 s wall-clock overshoot).

### Added — fixture

- `VALID_CONSTRAINTS` — shared constraints fixture used by the new
  constraint-compatibility test.
