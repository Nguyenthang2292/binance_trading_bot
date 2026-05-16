# Gemini Filter Auto-Tuning - Design Document

**Version:** 1.0  
**Date:** 2026-05-16  
**Status:** Proposed  

---

## 1. Goal

Auto-tune `quota` and `model_routing` for live gate using runtime telemetry, so the bot:

1. Keeps `enforce` stable (no prolonged `quota_exhausted`).
2. Uses cheaper/faster models when confidence quality is acceptable.
3. Escalates to stronger models only when needed.

---

## 2. Inputs

Source metrics come from `tmp/gemini_filter/cache/metrics/state.json` and logs:

- Cache: `sentiment.hit`, `sentiment.miss`, `sentiment.stale_hit`.
- Quota per model: `reserve_ok`, `reserve_reject_rpm`, `reserve_reject_rpd`, `reserve_reject_cooldown`, `cooldown_set`.
- Decision quality: distribution of `confidence`, `decision`, and later realized trade outcomes (future extension).

---

## 3. Tuning Loop

Run a controller every 15 minutes (or at scan-cycle boundary if simpler).

### Step 1: Aggregate window

Compute 1h and 24h rolling metrics per model:

- `reserve_success_rate = reserve_ok / (reserve_ok + reserve_reject_total)`
- `cooldown_rate = cooldown_set / reserve_attempts`
- `quota_pressure = reserve_reject_total / reserve_attempts`
- `p95_latency_ms` from analyzer logs (if available)

### Step 2: Score model utility

For each model, compute:

```text
utility = w1 * reserve_success_rate
        - w2 * quota_pressure
        - w3 * normalized_latency
        + w4 * decision_quality_proxy
```

Default weights:

- `w1=0.45`, `w2=0.30`, `w3=0.15`, `w4=0.10`

`decision_quality_proxy` is initially neutral (`0.5`) if no realized PnL attribution exists.

### Step 3: Update routing order

For sentiment and vision separately:

1. Sort candidates by utility descending.
2. Keep pinned fallback as last candidate.
3. If a model has `quota_pressure > 0.4` in the last 1h, demote one level.
4. If a model has `reserve_success_rate > 0.95` and lower latency than the current primary by >20%, promote one level.

### Step 4: Update effective quota budgets

Adjust dynamic safety factor per model:

```text
effective_safety = clamp(base_safety * (1 - quota_pressure_1h), min=0.35, max=0.90)
```

Then compute dynamic limits:

```text
dynamic_rpm = floor(configured_rpm * effective_safety)
dynamic_rpd = floor(configured_rpd * effective_safety)
```

### Step 5: Write runtime override

Persist active override to:

`tmp/gemini_filter/cache/autotune/active_override.json`

Fields:

- `generated_at`
- `expiry_at` (TTL 30m)
- `model_routing_override`
- `quota_override`
- `reason_summary`

Analyzer loads this override first, then merges with static config.

---

## 4. Safety Rails

1. Never remove pinned fallback models.
2. Never set dynamic safety above static safety.
3. Never enable Pro escalation for sentiment auto-route.
4. If override generation fails, keep last valid override until TTL expires.
5. If no valid override, fallback to static config immediately.
6. If `quota_exhausted` occurs for more than 3 consecutive cycles in enforce, force emergency profile:
   - disable Pro escalation
   - sentiment route -> cheapest model first
   - safety factor = `0.5`

---

## 5. Rollout Plan

1. Phase A: Observe-only.
   - Compute and log recommended override.
   - Do not apply automatically.
2. Phase B: Auto-apply in shadow.
   - Apply override only when `gemini_filter.mode != enforce`.
3. Phase C: Auto-apply in enforce with emergency rollback.
   - If block rate spikes > configured threshold, rollback to last stable override.

---

## 6. Test Plan

1. Unit tests for scoring and reordering logic.
2. Unit tests for dynamic safety factor boundaries.
3. Integration test: simulated quota pressure causes model demotion.
4. Integration test: stable fast model causes promotion.
5. Integration test: emergency profile activates on repeated quota exhaustion.

---

## 7. Acceptance Criteria

1. Routing order changes are deterministic given same metric window.
2. Dynamic quota never exceeds static quota limits.
3. Missing telemetry does not crash analyzer or routing.
4. Override TTL expiration reverts to static config safely.
5. In enforce mode, sustained quota pressure reduces `quota_exhausted` events after tuning convergence.

