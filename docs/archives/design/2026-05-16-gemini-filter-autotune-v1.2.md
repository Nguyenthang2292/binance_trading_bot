# Gemini Filter Auto-Tuning - Design Document

**Version:** 1.2  
**Date:** 2026-05-18  
**Status:** ✅ DONE - Implemented  

---

## 1. Goal

Auto-tune `quota` and `model_routing` for the Gemini live gate using local runtime telemetry, so the bot:

1. Keeps `enforce` stable with fewer repeated `quota_exhausted` cycles.
2. Uses cheaper/faster models when quality is acceptable.
3. Escalates to stronger models only when the configured vision escalation policy requires it.
4. Falls back to static config immediately when telemetry, override, or controller state is missing or invalid.

This design does **not** use `shadow` mode. The current C++ config parser removes `shadow` and forces unsupported modes to `enforce`, so rollout must be controlled by an explicit autotune feature flag instead of `gemini_filter.mode`.

---

## 2. Current Context

Existing implementation already provides:

- Static model routing from `gemini_filter.model_routing`.
- File-backed quota guards with RPM/RPD/cooldown buckets.
- Cache and quota counters in `tmp/gemini_filter/cache/metrics/state.json`.
- Per-request analyzer logs including route, decision, confidence, model reserve status, and latency.
- Fail-closed scan-cycle gate behavior on `quota_exhausted` in `enforce`.

Implementation closed these original gaps:

- Metrics are cumulative counters, not rolling time windows.
- Analyzer does not load runtime overrides.
- Routing order is static except for per-request quota rejection fallback.
- Quota `safety_factor` is static.
- There is no controller, override schema, TTL validation, or emergency profile state.

Manual API validation on 2026-05-18 confirmed the configured models are visible to the active Google API credentials:

- `gemini-2.5-flash-lite`
- `gemini-2.5-flash`
- `gemini-3.1-flash-lite`
- `gemini-3.1-pro-preview`

Smoke calls also confirmed JSON structured output and Google Search grounding work on `gemini-2.5-flash-lite`. The autotune controller must still avoid live Gemini API calls during normal tuning; it should use local telemetry only.

---

## 3. Configuration

Add an `autotune` block under `gemini_filter`:

```json
{
  "gemini_filter": {
    "autotune": {
      "enabled": false,
      "mode": "observe",
      "interval_seconds": 900,
      "controller_timeout_seconds": 60,
      "override_ttl_seconds": 1800,
      "min_samples_per_model": 10,
      "quota_pressure_demote_threshold": 0.40,
      "success_rate_promote_threshold": 0.95,
      "latency_promote_improvement_pct": 20,
      "max_block_rate_increase_pct": 10,
      "block_rate_window_seconds": 300,
      "bucket_retention_seconds": 90000,
      "emergency_quota_exhausted_cycles": 3,
      "weights": {
        "reserve_success_rate": 0.45,
        "quota_pressure": 0.30,
        "normalized_latency": 0.15,
        "decision_quality_proxy": 0.10
      }
    }
  }
}
```

Supported `mode` values:

| Mode | Behavior |
| --- | --- |
| `observe` | Compute, validate, and log recommended overrides, but do not write `active_override.json`. |
| `apply` | Compute, validate, and write `active_override.json`; analyzer may apply it even in `enforce`. |
| `disabled` | Do not run the controller and ignore existing active override. |

Default rollout starts with `enabled=false` and `mode=observe`. Move to `enabled=true`, `mode=apply` only after observe logs match expectations.

---

## 4. Telemetry Model

The controller needs rolling windows. Replace or extend the current cumulative metrics with append-safe time buckets.

### 4.1 Bucket Path

Persist telemetry buckets under:

```text
tmp/gemini_filter/cache/metrics/buckets/YYYY-MM-DDTHH-mm.json
```

Each bucket covers one minute in UTC. Writes must use the existing file-lock pattern and atomic replace.

### 4.2 Bucket Fields

Minimum bucket schema:

```json
{
  "schema_version": 1,
  "bucket_start_utc": "2026-05-18T10:15:00Z",
  "cache": {
    "sentiment": {
      "hit": 0,
      "miss": 0,
      "stale_hit": 0
    }
  },
  "quota": {
    "gemini-2.5-flash-lite": {
      "reserve_ok": 0,
      "reserve_reject_rpm": 0,
      "reserve_reject_rpd": 0,
      "reserve_reject_cooldown": 0,
      "cooldown_set": 0
    }
  },
  "analyzer": {
    "gemini-2.5-flash-lite": {
      "sentiment_calls": 0,
      "vision_calls": 0,
      "latency_stats": {
        "count": 0,
        "sum_ms": 0,
        "min_ms": 0,
        "max_ms": 0,
        "histogram_bucket_ms": 100,
        "histogram": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
      }
    }
  },
  "decisions": {
    "allow": 0,
    "block": 0,
    "quota_exhausted": 0,
    "component_error": 0
  }
}
```

Keep the existing cumulative `state.json` for simple diagnostics, but do not use it for rolling-window tuning.

### 4.3 Aggregated Metrics

For 1h and 24h windows, compute per model:

```text
reserve_attempts = reserve_ok + reserve_reject_rpm + reserve_reject_rpd + reserve_reject_cooldown
reserve_reject_total = reserve_reject_rpm + reserve_reject_rpd + reserve_reject_cooldown
reserve_success_rate = reserve_ok / reserve_attempts
cooldown_rate = cooldown_set / reserve_attempts
quota_pressure = reserve_reject_total / reserve_attempts
p95_latency_ms = p95(latency histogram)
```

If `reserve_attempts < min_samples_per_model`, mark the model as `insufficient_sample` and keep its existing relative order unless emergency rules apply.

Latency must be recorded as pre-aggregated stats, not as raw arrays. Each request updates `count`, `sum_ms`, `min_ms`, `max_ms`, and exactly one histogram bin. The last histogram bin is an overflow bucket for latencies above the explicit range. This keeps per-request write size bounded and avoids sorting raw latency samples during controller runs.

### 4.4 Bucket Retention

The controller only reads 1h and 24h windows. After each successful or failed controller run, it should delete metric bucket files older than `bucket_retention_seconds`.

Default retention is 90,000 seconds: 24h plus a 1h buffer. Cleanup must:

1. Use file mtime or parsed bucket timestamp, not filename lexicographic order alone.
2. Skip files currently locked by writers.
3. Never delete the current minute bucket.
4. Treat cleanup failure as non-fatal.

---

## 5. Tuning Controller

Run `tools/gemini_filter/autotune.py` as an isolated, non-blocking subprocess launched by the C++ main process.

The main process owns scheduling, but the controller owns tuning. At scan-cycle boundary, or from the existing main event loop timer, C++ checks `gemini_filter.autotune.enabled` and `interval_seconds`. If a run is due, it starts:

```text
python -m tools.gemini_filter.autotune --config config.json --runtime-base-dir tmp/gemini_filter
```

Lifecycle rules:

1. The bot must not block waiting for the controller to finish.
2. The controller process must not share mutable in-memory state with the bot.
3. A controller crash, non-zero exit, timeout, invalid output, or invalid override must not stop trading logic.
4. C++ keeps a lightweight process handle or PID state and polls it on later loop ticks.
5. If the controller exceeds `controller_timeout_seconds`, C++ terminates it and logs a warning.
6. A lock file under `tmp/gemini_filter/cache/autotune/controller.lock` prevents overlapping controller runs.
7. If another controller run is active, skip the new run and log once per interval.

The controller is local-only and must not call Gemini APIs.

Controller pipeline:

1. Load static config from the analyzer input shape or `config.json`.
2. Load 1h and 24h telemetry buckets.
3. Aggregate per-model and per-component metrics.
4. Build a recommended route and quota override.
5. Validate safety rails.
6. Update persistent autotune state.
7. Atomically write `last_recommendation.json`.
8. In `observe`, log the recommendation and stop before writing `active_override.json`.
9. In `apply`, atomically write `active_override.json`.
10. Cleanup metric buckets older than retention.

Controller output is operational logging only. The bot should not parse stdout to make trading decisions.

---

## 6. Scoring

For each candidate model:

```text
utility = w1 * reserve_success_rate
        - w2 * quota_pressure
        - w3 * normalized_latency
        + w4 * decision_quality_proxy
```

Default weights come from `gemini_filter.autotune.weights`:

- `w1=0.45`
- `w2=0.30`
- `w3=0.15`
- `w4=0.10`

Rules:

1. `decision_quality_proxy` starts at `0.5` until realized trade outcome attribution exists.
2. `normalized_latency` is computed against the slowest candidate in the same component and window.
3. If latency is unavailable, use neutral latency `0.5`.
4. If a model has insufficient samples, keep neutral utility and do not promote it automatically.
5. Use stable sort with original configured order as the final tie breaker.

---

## 7. Routing Update

Tune sentiment and vision routes separately.

Rules:

1. Start from static configured candidates.
2. Never remove the pinned fallback model.
3. Keep the pinned fallback as the last candidate unless it is the only candidate.
4. Sort non-fallback candidates by utility descending.
5. If a model has `quota_pressure > quota_pressure_demote_threshold` in the 1h window, demote one level.
6. If a non-primary model has `reserve_success_rate > success_rate_promote_threshold` and `p95_latency_ms` at least `latency_promote_improvement_pct` faster than the current primary, promote one level.
7. Do not enable Pro escalation for sentiment. Sentiment routes may contain Pro only if static config already pinned it as fallback.
8. Preserve the static vision `pro_escalation_enabled`, `pro_escalation_min_score`, and `pro_escalation_max_score` unless emergency profile disables escalation.

---

## 8. Dynamic Quota

For each configured model:

```text
effective_safety = clamp(
    base_safety * (1 - quota_pressure_1h),
    min=0.35,
    max=base_safety
)
```

Then compute:

```text
dynamic_rpm = max(1, floor(configured_rpm * effective_safety))
dynamic_rpd = max(1, floor(configured_rpd * effective_safety))
```

Rules:

1. Dynamic limits must never exceed static configured limits.
2. Dynamic safety must never exceed static `quota.safety_factor`.
3. Missing model telemetry means use static limits.
4. RPD pressure should decay slowly. Prefer 24h pressure for explanatory logs, but use 1h pressure for the immediate dynamic limit.
5. Do not rewrite static `config.json`; dynamic quota lives only in the runtime override.

---

## 9. Runtime Override

Persist active override to:

```text
tmp/gemini_filter/cache/autotune/active_override.json
```

### 9.1 Schema

```json
{
  "schema_version": 1,
  "generated_at": "2026-05-18T10:15:00Z",
  "expiry_at": "2026-05-18T10:45:00Z",
  "override_type": "normal",
  "model_routing_override": {
    "sentiment": {
      "candidates": ["gemini-2.5-flash-lite", "gemini-2.5-flash", "gemini-3.1-pro-preview"]
    },
    "vision": {
      "candidates": ["gemini-3.1-flash-lite", "gemini-2.5-flash", "gemini-3.1-pro-preview"],
      "pro_escalation_enabled": true,
      "pro_escalation_min_score": 0.45,
      "pro_escalation_max_score": 0.65
    }
  },
  "quota_override": {
    "safety_factor_by_model": {
      "gemini-2.5-flash-lite": 0.62
    },
    "models": {
      "gemini-2.5-flash-lite": {
        "rpm": 12,
        "rpd": 620
      }
    }
  },
  "reason_summary": [
    "sentiment primary kept: gemini-2.5-flash-lite reserve_success_rate=0.98 quota_pressure=0.02",
    "vision primary demoted one level: gemini-3.1-flash-lite quota_pressure=0.44"
  ]
}
```

The override file does not contain controller `mode`. `observe`, `apply`, and `disabled` are read only from static config. `override_type` is informational and may be `normal`, `emergency`, or `rollback`.

### 9.2 Analyzer Runtime Merge Rules

Analyzer override loading is runtime behavior, not startup-only behavior.

The current architecture invokes the Python analyzer per Gemini evaluation, so each analyzer invocation should load and validate `active_override.json` before model resolution, route construction, and quota manager construction. If a future long-lived analyzer process is introduced, it must reload the override at least once per request, using file mtime and TTL validation, rather than caching an override for process lifetime.

Analyzer should:

1. Check `gemini_filter.autotune.enabled`.
2. If disabled, ignore `active_override.json`.
3. Load `active_override.json` from the runtime base directory.
4. Reject override if missing, expired, corrupt, unsupported schema, or unsafe.
5. Merge override over static `model_routing` and `quota`.
6. Fall back to static config immediately on any override load or validation error.

Static config remains the source of truth. Override may narrow quota and reorder configured candidates, but it must not introduce models absent from static config.

### 9.3 Atomic Write

Controller writes `active_override.json.tmp`, fsyncs if practical, then atomically replaces `active_override.json`. Invalid generated overrides must not replace the last valid override.

Controller also writes the latest recommendation to:

```text
tmp/gemini_filter/cache/autotune/last_recommendation.json
```

`last_recommendation.json` uses the same schema as `active_override.json` plus validation details. It is written in both `observe` and `apply` modes for debugging and comparison. Analyzer must never apply `last_recommendation.json`.

---

## 10. Emergency Profile

If `quota_exhausted` occurs for more than `emergency_quota_exhausted_cycles` consecutive telemetry windows, controller writes an emergency override with short TTL.

The consecutive counter must be persisted because the controller is a short-lived subprocess. Store controller state at:

```text
tmp/gemini_filter/cache/autotune/state.json
```

Minimum state schema:

```json
{
  "schema_version": 1,
  "consecutive_quota_exhausted_cycles": 4,
  "last_updated": "2026-05-18T10:16:01Z",
  "active_override_generated_at": "2026-05-18T10:15:00Z",
  "active_override_baseline": {
    "block_rate_24h_before_apply": 0.18
  }
}
```

Implemented note: telemetry buckets persist decision counters, including `quota_exhausted`. The controller updates the persisted consecutive counter from recent bucketed quota-exhausted decisions instead of a separate per-cycle `cycle_id` event.

Emergency behavior:

1. Disable vision Pro escalation.
2. Put cheapest configured sentiment model first.
3. Put cheapest configured vision model first if present.
4. Set effective safety to `min(static_safety_factor, 0.5)` for every model.
5. Preserve pinned fallback as the final candidate.
6. Include `reason_summary` explaining the consecutive cycle count.

Emergency override exits automatically through TTL expiry when the next controller run sees normal pressure.

---

## 11. Safety Rails

1. Never remove pinned fallback models.
2. Never introduce a model that is not present in static config.
3. Never set dynamic RPM/RPD above static RPM/RPD.
4. Never set dynamic safety above static safety.
5. Never enable Pro escalation for sentiment.
6. Never enable vision Pro escalation if static config disabled it.
7. If override generation fails, keep the last valid override until TTL expires.
8. If no valid override exists, use static config.
9. If telemetry is insufficient, keep static routing order.
10. If block rate increases above `max_block_rate_increase_pct` after applying an override, rollback by writing a static-equivalent override with short TTL or by deleting the active override atomically.

Rollback detection:

```text
block_rate_now = blocks in last block_rate_window_seconds / decisions in last block_rate_window_seconds
baseline = 24h block_rate immediately before active override generated_at
rollback if block_rate_now > baseline * (1 + max_block_rate_increase_pct / 100)
```

Default `block_rate_window_seconds` is 300 seconds. If the 5-minute window has insufficient decision samples, do not rollback from block-rate alone; rely on quota emergency rules instead. Store the baseline in `autotune/state.json` when an apply-mode override is written.

---

## 12. Rollout Plan

No shadow mode is used.

### Phase A - Observe

Config:

```json
{
  "enabled": true,
  "mode": "observe"
}
```

Behavior:

- Build telemetry buckets.
- Compute recommended overrides.
- Log recommendations and validation results.
- Write `last_recommendation.json` for debugging.
- Do not write `active_override.json`.

Exit criteria:

- At least 24h of telemetry.
- Recommendations are deterministic across repeated runs with same buckets.
- No controller crashes on missing/corrupt telemetry buckets.

### Phase B - Apply With Conservative Limits

Config:

```json
{
  "enabled": true,
  "mode": "apply"
}
```

Behavior:

- Apply override in `enforce`.
- Keep TTL at 30 minutes.
- Keep emergency profile enabled.
- Use conservative thresholds and require `min_samples_per_model`.

Exit criteria:

- `quota_exhausted` cycles decrease or stay flat.
- Block rate does not spike beyond configured threshold.
- Override expiry safely reverts to static config.

### Phase C - Tune Thresholds

Behavior:

- Adjust thresholds and weights only after Phase B has stable telemetry.
- Keep static config as fallback.
- Do not remove safety rails.

---

## 13. Test Plan

1. Unit test scoring with deterministic tie breakers.
2. Unit test insufficient sample keeps static order.
3. Unit test quota pressure demotes one level.
4. Unit test stable fast model promotes one level.
5. Unit test dynamic safety never exceeds static safety.
6. Unit test dynamic RPM/RPD never exceeds static limits.
7. Unit test corrupt override falls back to static config.
8. Unit test expired override falls back to static config.
9. Integration test analyzer merges valid override before routing/quota construction.
10. Integration test emergency profile activates after repeated enforce quota exhaustion.
11. Integration test observe mode logs recommendation but does not write active override.
12. Integration test apply mode writes valid active override atomically.
13. Integration test controller cold start with no bucket files.
14. Integration test block-rate rollback after a bad override.
15. Integration test bucket cleanup skips locked/current bucket files.
16. Integration test controller timeout does not block or stop the bot.
17. Integration test analyzer reloads a newly written override on the next invocation.

Manual smoke test:

1. Load `.env`.
2. List Gemini models through `google-genai`.
3. Confirm configured models are available.
4. Send a minimal JSON-score request to the cheapest configured model.
5. Send a minimal Google Search grounding request.

Manual smoke tests validate API connectivity only. They are not a substitute for unit and integration tests, and they should not run as part of normal autotune control.

---

## 14. Acceptance Criteria

1. Routing order changes are deterministic given the same telemetry buckets.
2. Dynamic quota never exceeds static quota limits.
3. Missing, corrupt, or insufficient telemetry does not crash analyzer or controller.
4. Missing, corrupt, unsafe, or expired override reverts to static config safely.
5. Analyzer applies a valid override before building model route and quota manager.
6. Analyzer reloads override state per invocation or per request, not process lifetime only.
7. Observe mode writes `last_recommendation.json` but never writes `active_override.json`.
8. Apply mode writes only schema-valid, safety-checked overrides.
9. Controller runs as an isolated subprocess; crash or timeout does not stop the bot.
10. Emergency override activates from persisted state after repeated enforce quota exhaustion and expires by TTL.
11. Block-rate rollback uses a defined 5-minute window and pre-apply 24h baseline.
12. Bucket cleanup removes old buckets without deleting current or locked files.
13. In enforce mode, sustained quota pressure reduces or does not increase `quota_exhausted` events after tuning convergence.
