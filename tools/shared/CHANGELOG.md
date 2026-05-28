# Changelog — tools/shared

All notable changes to the shared helpers in this folder are documented here.

Format inspired by [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## 2026-05-26 — `gemini_key_manager.py`

Driven by code review of `gemini_key_manager.py`. All changes preserve the public
surface (`GeminiKey`, `GeminiKeyManager`, `run_with_rotation`,
`_is_retryable_key_error`, `_split_key_list`, `_HTTP_TIMEOUT_MS`).

### Fixed — Critical

- **Bare-except in `_claim_start_index` silently degraded rotation to index 0.**
  Narrowed to `(TimeoutError, OSError, ValueError, TypeError, json.JSONDecodeError)`,
  added a warning log, and switched the fallback from a deterministic `0` to
  `random.randrange(len(keys))` so concurrent processes still spread load when
  the state file is unreachable.
- **Lock file was not crash-safe (stale lock locked everyone out for 2s, then
  fell back to index 0).** `_file_lock` now reclaims a marker file whose `mtime`
  is older than `_LOCK_STALE_SECONDS` (30 s) and retries, so a killed process
  cannot deadlock subsequent ones.
- **Default `state_dir` was CWD-relative (`tmp/gemini_key_manager`).** Switched
  to an absolute path derived from `tempfile.gettempdir()`. Explicit `state_dir`
  arguments and the `GEMINI_KEY_MANAGER_STATE_DIR` env var are now normalised
  via `expanduser().resolve(strict=False)` so all paths are absolute regardless
  of caller CWD.

### Fixed — Correctness & resource handling

- **`genai.Client` was never closed**, leaking connection pools per rotation
  attempt. `run_with_rotation` now closes the client in a `finally` block; close
  errors are logged but do not interrupt the main flow.
- **Lost root cause when all keys exhausted.** Final `RuntimeError` is now
  raised with `from last_error` so `__cause__` carries the original API error.
- **Wall-clock lock deadline.** `_file_lock` now uses `time.monotonic()` for
  the wait budget so NTP adjustments cannot bypass the timeout.
- **`_write_state` lacked durability hint.** Added `flush()` + `os.fsync()` on
  the temp file before `replace()`, with an inline comment documenting that
  directory fsync is intentionally skipped (advisory state).

### Changed — Retryable-error classification

- `_is_retryable_key_error` now rotates **only on key-scoped failures**
  (401 / 429 + quota/rate/api-key/unauthorised patterns). 5xx, timeout,
  `unavailable`, and `deadline_exceeded` are treated as **shared failure
  domains** and fail fast so callers can apply global backoff once instead of
  fanning out across N keys.
- The text fallback now uses pre-compiled regex patterns with `\b` word
  boundaries (`_KEY_ERROR_PATTERNS`) rather than loose `in` substring matching,
  so messages like `"sampling rate mismatch"` no longer trigger key rotation.

### Changed — State & key collection

- **`_keyset_digest` now hashes only key values, not names.** Renaming an env
  var (e.g. `GEMINI_API_KEY_1` → `GEMINI_API_KEY`) no longer invalidates the
  persisted `next_index` and resets the round-robin position.
- **Packed-key naming.** `GEMINI_API_KEYS` entries are now labelled
  `GEMINI_API_KEYS_1`, `GEMINI_API_KEYS_2`, … (previously `GEMINI_API_KEYS[1]`)
  to keep names log-friendly and free of shell-quoting hazards.

### Removed

- Unreachable `if last_error is None` branch in `run_with_rotation` —
  `__init__` rejects empty key sets, so the loop always runs at least once.
