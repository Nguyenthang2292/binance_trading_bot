# Changelog — tools/gemini_filter

All notable changes to the Gemini filter package are documented here.

Format inspired by [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## 2026-05-26 — Code review remediation

Driven by code review of the `gemini_filter` package. Structural changes are
covered by tests under `tests/python/test_gemini_client.py`,
`test_cache_store.py`, `test_analyzer.py`, and `test_autotune.py`. The CLI
contract (single-arg `python -m tools.gemini_filter.gemini_filter <input.json>`,
exit code `0` with structured JSON on stdout) is unchanged.

### Fixed — Critical

- **JSON parse failure when Google Search grounding was enabled.** With
  `sentiment_search_then_score=false` (the default), `generate_json_score`
  requested freeform text from Gemini (schema mode is incompatible with
  grounding) and then called `json.loads` on the response — but the sentiment
  prompt never instructed the model to return JSON, so every single-step
  sentiment call surfaced as `quota_exhausted` / `component_error`. The
  sentiment prompt in `analyzer._build_sentiment_prompt` now appends a strict
  JSON-only instruction, and `gemini_client.generate_json_score` additionally
  appends a `_JSON_ONLY_INSTRUCTION` guard whenever `use_google_search=True` and
  the content is a string.
- **Histogram data loss in autotune aggregation.** When two buckets reported
  histograms of different lengths, `_aggregate_model_stats` zeroed out the
  previously accumulated counts before merging the new bucket. Replaced with a
  length-tolerant merge that extends to `max(existing_len, incoming_len)` and
  sums element-wise. Regression test asserts `[1,2,3] + [4,5,6,7,8] → [5,7,9,7,8]`.
- **Stale `cache_store.file_lock` wedged the metrics layer indefinitely.** The
  previous lock used `O_CREAT | O_EXCL` with no stale-lock recovery, so a
  killed process left a lock file behind and every subsequent `MetricsStore`
  operation raised `TimeoutError` until manual cleanup. Ported the stale-lock
  reclaim logic from `tools/shared/gemini_key_manager._file_lock`: locks older
  than `_LOCK_STALE_SECONDS` (30 s) are unlinked and retried; the wait deadline
  uses `time.monotonic()`. `PermissionError` is now also caught on the
  `os.open` attempt for Windows compatibility.

### Fixed — Correctness & input validation

- **Daemon analyzer threads leaked silently on timeout.** The 120 s deadline
  enforcement remained, but a hung sentiment/vision thread used to vanish into
  a `Block` with no diagnostic. Threads are now named
  (`analyzer-sentiment-<eval_id>`, `analyzer-vision-<eval_id>`) and the
  timeout path logs `alive_threads=...` at WARNING. The `_deadline` private
  name was renamed to `deadline`.
- **`klines` payload shape was unchecked.** `_validate_input` (in
  `gemini_filter.py`) and `_analyze_vision` (in `analyzer.py`) used to assume
  `data["klines"]` was a dict of timeframe → list-of-rows; a list payload
  surfaced as `analyzer_exception` instead of `invalid_input`. Validation now
  rejects non-dict `klines`, requires the primary timeframe to be present and
  non-empty, requires each row to be a dict carrying every field in
  `_REQUIRED_KLINE_FIELDS` (`open_time, open, high, low, close, volume`), and
  rejects `extra_tfs` that is not a list.
- **`chart_generator` swallowed schema errors as cryptic pandas exceptions.**
  Added `_validate_klines_shape`, and wrapped `pd.to_datetime(open_time)` plus
  every numeric `astype(float)` in explicit `RuntimeError("invalid_input: …")`
  guards so malformed klines surface with a clear error code.
- **`score` field parse raised `ValueError` instead of a structured
  `RuntimeError`.** `parse_score_payload` now wraps `float(payload["score"])`
  in `try/except (TypeError, ValueError)` and re-raises as
  `RuntimeError("Score must be numeric")`, matching the surrounding error
  contract.
- **Shallow copy in `_apply_autotune_override` leaked nested-dict references
  from the input payload.** Replaced `updated = dict(data)` with
  `copy.deepcopy(data)` so override merging cannot mutate the original
  `data["model_routing"]` / `data["quota"]` sub-trees observed by the caller.
- **Vision pro-tier escalation matched the substring `"-pro"`.** A
  hypothetical model name like `gemini-pro-mini-flash` would have been
  classified as a pro candidate. Introduced
  `_PRO_TIER_MODEL_PATTERN = r"^gemini-\d+(?:\.\d+)?-pro(?:-|$)"` and helper
  `_is_pro_tier_model`; regression test seeds a decoy candidate
  `gemini-pro-mini-flash` and asserts only `gemini-3.1-pro-preview` is selected
  for escalation.
- **Autotune emergency-mode counter could double-count the same incident.**
  The 5-minute decision window overlaps across consecutive controller runs, so
  the previous `consecutive_quota_exhausted_cycles` logic incremented every
  run where the window still contained an old failure, eventually crossing
  `emergency_quota_exhausted_cycles`. Counter now stores
  `last_quota_exhausted_5m` and increments only when the **current** count
  exceeds the previous run's count (delta-based detection).
- **`sentiment_cache_max_stale_seconds < sentiment_cache_ttl_seconds` was
  silently clamped.** Now emits a `WARNING` log identifying `eval_id`,
  `symbol`, the requested value, and the clamped TTL.
- **`save_entry` was unlocked and used a deterministic `.tmp` suffix.**
  Concurrent writers targeting the same cache key could race on the temp file.
  Added a per-key advisory lock (`<digest>.json.lock`) and switched to
  `tempfile.NamedTemporaryFile(dir=path.parent, suffix=".tmp")` so each writer
  gets a unique temp filename; on failure the temp file is unlinked.
- **Autotune controller lock had no stale-recovery.** A killed autotune run
  would block every subsequent cycle until manual cleanup; `run()` now passes
  `stale_seconds=5.0` to `file_lock`, matching the controller's 60-second
  cadence so a real crash unblocks the next cycle.

### Changed — Performance & maintainability

- **`QuotaManager.reserve` held the file lock across the rate-limit math.**
  Limit scaling (`_scaled_limit`) and the metrics counter increment are now
  computed outside the `with file_lock` block; the lock holds only the state
  read / token-bucket update / write, reducing contention under fan-out.
  `_scaled_limit` also makes the `math.floor` truncation explicit (previously
  the implicit `int()` cast).
- **`MetricsStore.incr_cache` / `incr_quota` did two lock round-trips per
  call.** Consolidated into `_incr_state_and_bucket`, which holds the lock
  once and updates both the rollup `state.json` and the per-minute bucket.
- **Duplicated `_parse_utc_iso` extracted.** `analyzer.py` and `autotune.py`
  both carried verbatim copies; introduced new module
  `tools/gemini_filter/time_utils.py` exporting `parse_utc_iso`, and both
  callers now import from it.
- **Autotune candidate-whitelist filter was duplicated for sentiment and
  vision.** Extracted helper `_filter_candidates_against_whitelist` in
  `analyzer.py` taking `static_candidates_raw`, `override_candidates_raw`,
  and `pinned_model`.
- **`gemini_filter.main()` usage-error path now documents the exit-0
  contract.** Inline comment notes that the C++ parent reads structured JSON
  from stdout instead of the shell exit status.

### Added

- `time_utils.py` — shared UTC-ISO parsing for the package.
- `_JSON_ONLY_INSTRUCTION` constant in `gemini_client.py`, exported via the
  prompt suffix path.
- `_is_pro_tier_model` helper and `_PRO_TIER_MODEL_PATTERN` regex in
  `analyzer.py`.
- `_filter_candidates_against_whitelist` helper in `analyzer.py`.
- `_validate_klines_shape` helper in `chart_generator.py`.
- `_REQUIRED_KLINE_FIELDS` constant shared between `gemini_filter.py` input
  validation and `chart_generator.py` shape validation.
- Regression tests:
  - `test_cache_store.test_file_lock_recovers_stale_lock`
  - `test_cache_store.test_file_lock_times_out_for_fresh_lock`
  - `test_cache_store.test_save_entry_concurrent_writers_same_key`
  - `test_autotune.test_aggregate_model_stats_preserves_histogram_when_lengths_change`
  - `test_autotune.test_emergency_triggers_at_threshold_not_after`
  - `test_analyzer.test_sentiment_single_step_uses_legacy_search_json_path`
  - `test_analyzer.test_analyze_vision_rejects_non_object_klines`
  - `test_analyzer.test_vision_escalation_filters_to_pro_tier_candidates`
