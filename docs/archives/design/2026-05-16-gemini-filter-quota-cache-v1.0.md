# Gemini Filter Quota And Cache - Design Document

**Version:** 1.0  
**Date:** 2026-05-16  
**Status:** ✅ DONE - Implemented  

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-05-16 | Initial design for Gemini result cache, decoupled sentiment cache, 1h model-resolution TTL, live model routing, internal RPM/RPD quota buckets, and fail-closed scan-cycle gate closure |

---

## 1. Problem

The current Gemini filter is fail-closed in `enforce`, but the scan loop can still produce a large number of candidate signals after the Gemini budget is already exhausted. That causes log flood and wasted CPU, and it makes the runtime look as if signals are passing through the Gemini filter even when order placement is blocked.

The current hot path also spends Gemini capacity inefficiently:

1. Each signal can run sentiment and vision even if the same symbol/direction/candle was evaluated moments ago.
2. Sentiment is coupled to every signal, even though symbol-level market sentiment changes much slower than chart candles.
3. Model resolution may call `models.list()` in the live path.
4. The same expensive model is used for both sentiment and vision.
5. Key rotation does not solve project-level quota pressure because Gemini API rate limits are applied per project, not per API key.
6. The current scan-cycle budget is a fixed evaluation count, not a quota-aware RPM/RPD guard.

---

## 2. External Constraints

These constraints are based on the Gemini API documentation as of 2026-05-16:

| Constraint | Design impact |
|---|---|
| Rate limits are measured by RPM, TPM, and RPD. Exceeding any dimension can trigger a rate-limit error. | Track request budget at least by RPM and RPD; keep TPM as an extension point. |
| Rate limits apply per Google Cloud project, not per API key. | Do not treat multiple API keys from the same project as extra quota. Key rotation is for temporary key failures, not capacity scaling. |
| RPD resets at midnight Pacific time. | Daily bucket reset must use Pacific-time day boundaries, not local machine midnight. |
| Preview/experimental models have more restricted limits. | Prefer stable Flash/Lite models in the live gate and reserve Pro Preview only for vision escalation or offline analysis. |
| Batch API is lower cost but asynchronous and intended for non-urgent workloads. | Do not use Batch API for live order gating; it is suitable for backtests and offline evaluation only. |
| Gemini context caching still consumes standard `GenerateContent` rate limits. | Gemini context caching can reduce cost, but it does not replace local result/sentiment caches for rate protection. |

References:

- https://ai.google.dev/gemini-api/docs/rate-limits
- https://ai.google.dev/gemini-api/docs/caching
- https://ai.google.dev/gemini-api/docs/thinking
- https://ai.google.dev/gemini-api/docs/batch-api

---

## 3. Goals

1. Cache final Gemini decisions for the same symbol, direction, timeframe, and chart state.
2. Cache sentiment independently from signal evaluation.
3. Cache model resolution for 1 hour with stale fallback.
4. Route live gate calls to cheaper/faster models first, while allowing vision to escalate to a Pro model when configured.
5. Add internal quota guards for RPM and RPD, with room for TPM later.
6. In `enforce`, close the Gemini gate for the rest of the scan cycle once quota or cycle budget is exhausted.
7. Log `gemini gate closed for cycle` once per cycle and skip later strategy evaluation/order attempts until the next scan cycle.

---

## 4. Non-Goals

1. Do not replace the current C++/Python subprocess bridge in this design. A long-lived Python worker can be a later optimization.
2. Do not use Batch API for live order gating.
3. Do not claim that multiple API keys increase quota unless they belong to separate Google Cloud projects and are configured as separate quota pools.
4. Do not make Gemini the first filter. Local sizing, order cap, exposure, and position-tracker checks should still run before spending Gemini capacity.

---

## 5. Target Runtime Flow

Current rough flow:

```text
WorkItem
  -> strategy.evaluate()
  -> candidate signal log
  -> position/order/exposure checks
  -> Gemini evaluate
  -> order placement
```

Target flow:

```text
runScanCycle()
  -> reset scan-cycle Gemini gate
  -> for WorkItem:
       if Gemini enforce gate is closed:
           skip before strategy.evaluate()
       strategy.evaluate()
       local filters
       final Gemini result cache lookup
       if cache miss:
           quota/cycle budget check
           Gemini evaluate through Python
       if Gemini block/error/quota exhausted:
           block order
           close gate for cycle only for budget/quota exhaustion in enforce
       else:
           place order
```

The key behavior change is the early skip:

```text
if mode == enforce and geminiGate.closedForCycle():
    return before strategy evaluation
```

This removes candidate-signal log spam after the fail-closed gate has already determined that no more orders can be opened in this scan cycle.

---

## 6. C++ Scan-Cycle Gate

Add a small state object owned by `SignalEngine`:

```cpp
struct GeminiCycleGate {
    bool closed{false};
    std::string reason;
    std::string firstSymbol;
    std::string firstTf;
    int skippedItems{0};
};
```

Reset it at the start of every `runScanCycle()`.

Close it through one helper only:

```cpp
void SignalEngine::closeGeminiGateForCycle(
    std::string reason,
    std::string_view symbol,
    std::string_view tf);
```

The helper logs once:

```text
gemini gate closed for cycle reason="quota_exhausted" first_symbol=BLUAIUSDT first_tf=1h policy=fail_closed
```

After closure, `processItem()` skips before `strategy.evaluate()` when:

```text
gemini_filter.enabled == true
and gemini_filter.mode == enforce
and m_geminiCycleGate.closed == true
```

At end of cycle, optionally log a summary:

```text
gemini gate cycle summary closed=true reason="quota_exhausted" skipped_items=842
```

---

## 7. Final Gemini Result Cache

The final result cache prevents repeated full Gemini evaluations for the same chart state.

### Owner

Use an in-memory C++ cache in `GeminiFilterController` or a small `GeminiResultCache` owned by `SignalEngine`.

Disk persistence is optional. The first implementation should be memory-only because stale persisted trading decisions are more dangerous than recomputing after restart.

### Key

Do not key only by symbol/timeframe. The chart can update inside the same candle. Use a fingerprint:

```text
gemini_result:v1:
  symbol:
  direction:
  primary_tf:
  chart_fingerprint:
  model_route_hash:
  prompt_version
```

Where:

| Field | Source |
|---|---|
| `symbol` | Work item symbol, e.g. `BLUAIUSDT` |
| `direction` | `Long` or `Short` |
| `primary_tf` | Signal interval |
| `chart_fingerprint` | Stable hash of serialized primary and extra TF OHLCV payload used for chart generation |
| `model_route_hash` | Hash of sentiment/vision model route config and thinking config |
| `prompt_version` | Static version bumped whenever prompts or scoring schema change |

Store raw scores and metadata:

```json
{
  "decision": "Allow",
  "confidence": 0.72,
  "sentiment_score": 0.64,
  "vision_score": 0.80,
  "reason": "Sentiment and vision evaluated",
  "error_code": null,
  "created_at_ms": 1778912345000,
  "expires_at_ms": 1778914145000,
  "model_route": {
    "sentiment_model": "gemini-2.5-flash-lite",
    "vision_model": "gemini-3.1-flash-lite"
  }
}
```

### TTL

Default:

```json
"result_cache_ttl_seconds": 1800
```

The fingerprint makes the TTL mostly a memory bound. A new chart state should produce a different key.

### Error caching

Do not cache generic transport errors for the full TTL.

Use short negative TTLs:

| Error | TTL |
|---|---:|
| `quota_exhausted` | until quota manager retry time or scan-cycle end |
| `timeout` | 60s |
| `component_error` | 60s |
| validation/parse error | 300s, with alert-level log |

---

## 8. Decoupled Sentiment Cache

Sentiment should be evaluated per asset/direction/time bucket, not per signal.

### Owner

Because the current Python subprocess exits after every evaluation, Python must use a small file-backed cache under:

```text
tmp/gemini_filter/cache/sentiment/
```

Use atomic write + lock file:

1. Compute cache key.
2. Acquire lock for that key.
3. Re-check existing cache file after lock.
4. If valid, return it.
5. If missing/expired, reserve quota and call Gemini.
6. Write JSON to temp file and rename atomically.

### Key

```text
sentiment:v1:
  base_asset:
  direction:
  sentiment_model:
  google_search_enabled:
  prompt_version:
  time_bucket
```

Example:

```text
sentiment:v1:BLUAI:Long:gemini-2.5-flash-lite:true:s1:2026-05-16T05
```

### TTL

Default:

```json
"sentiment_cache_ttl_seconds": 3600
```

Recommended policy:

| Signal TF | Sentiment TTL |
|---|---:|
| `30m` | 1800s |
| `1h` | 3600s |
| `4h` | 7200s |
| `1d` | 14400s |

The first implementation can use one global TTL. Timeframe-aware TTL can be added later.

### Stale fallback

When Gemini sentiment quota is exhausted:

1. If stale sentiment exists and is younger than `sentiment_cache_max_stale_seconds`, use it and mark `sentiment_stale=true`.
2. If no usable stale result exists, return component error `quota_exhausted`.
3. C++ blocks the order in enforce.
4. C++ closes the scan-cycle gate only when the returned error is `quota_exhausted` or `budget_exhausted`.

---

## 9. Model Resolution Cache With 1h TTL

Model discovery is not allowed in the hot path on every signal.

### Owner

Use Python file-backed cache because `model_resolver.py` currently owns model discovery:

```text
tmp/gemini_filter/cache/model_resolution/
```

### Key

```text
model_resolution:v1:
  mode:
  allow_preview:
  tier:
  capability_set
```

Example:

```text
model_resolution:v1:latest_flash:false:flash:generateContent
```

### TTL

Default:

```json
"model_resolution_ttl_seconds": 3600
```

### Stale-While-Revalidate

If cache is expired but present:

1. Try to refresh only if quota is available.
2. If refresh fails and `fallback_on_error=true`, use stale cached result for up to:

```json
"model_resolution_max_stale_seconds": 86400
```

3. If no stale result exists, fall back to pinned models when `fallback_on_error=true`.
4. If `fallback_on_error=false`, return a blocking component error in enforce.

---

## 10. Model Routing For Live Gate

Live gate model selection should be component-specific.

### Default route

Recommended first-pass config:

```json
"model_routing": {
  "enabled": true,
  "sentiment": {
    "candidates": [
      "gemini-2.5-flash-lite",
      "gemini-2.5-flash",
      "gemini-3.1-flash-lite"
    ],
    "thinking": {
      "gemini_2_5_thinking_budget": 0,
      "gemini_3_thinking_level": "minimal"
    }
  },
  "vision": {
    "candidates": [
      "gemini-3.1-flash-lite",
      "gemini-2.5-flash",
      "gemini-3.1-pro-preview"
    ],
    "pro_escalation": {
      "enabled": true,
      "only_if_flash_confidence_between": [0.45, 0.65]
    },
    "thinking": {
      "gemini_2_5_thinking_budget": 0,
      "gemini_3_thinking_level": "low"
    }
  }
}
```

### Routing rules

1. Pick the first candidate model whose quota bucket has capacity.
2. If a candidate returns `429` or `RESOURCE_EXHAUSTED`, mark that model route as cooling down.
3. Move to the next configured candidate only if it has quota capacity.
4. Do not rotate API keys to bypass quota for the same project.
5. Vision can optionally escalate to Pro only when Flash returns an ambiguous score, e.g. `0.45 <= score <= 0.65`.
6. Sentiment should prefer cheap/fast models and should not escalate to Pro in the live path by default.

### Why not always Pro for vision?

Pro can be kept as an escalation model because chart interpretation can benefit from stronger multimodal reasoning. But using Pro for every chart quickly consumes preview/pro quota and increases latency. The live gate should spend Pro only when the cheaper model is uncertain.

---

## 11. Internal RPM/RPD Quota Manager

Add a file-backed quota manager in Python and a scan-cycle gate in C++.

Python owns API-call-level quota because it is the process that knows which component cache hit/miss occurs before each Gemini API request.

### State path

```text
tmp/gemini_filter/cache/quota/state.json
```

### Config

```json
"quota": {
  "enabled": true,
  "safety_factor": 0.70,
  "cooldown_seconds_on_429": 300,
  "models": {
    "gemini-2.5-flash-lite": {
      "rpm": 20,
      "rpd": 1000
    },
    "gemini-2.5-flash": {
      "rpm": 10,
      "rpd": 500
    },
    "gemini-3.1-flash-lite": {
      "rpm": 10,
      "rpd": 500
    },
    "gemini-3.1-pro-preview": {
      "rpm": 2,
      "rpd": 50
    }
  }
}
```

The numbers above are placeholders. The operator must fill them from AI Studio for the active Google Cloud project.

Apply `safety_factor` before admission:

```text
effective_rpm = floor(configured_rpm * safety_factor)
effective_rpd = floor(configured_rpd * safety_factor)
```

### RPM bucket

Use a token bucket per model:

```text
capacity = effective_rpm
refill_rate_per_second = effective_rpm / 60
cost_per_generate_content = 1
```

Before each Gemini `generate_content` call:

```text
if bucket has >= 1 token:
    consume 1 token
else:
    return quota_exhausted with retry_after_seconds
```

### RPD bucket

Use a per-model daily counter:

```text
day_key = Pacific date, e.g. 2026-05-16 America/Los_Angeles
if used_today + 1 <= effective_rpd:
    increment
else:
    return quota_exhausted until next Pacific midnight
```

### 429 handling

When Gemini returns `429` or `RESOURCE_EXHAUSTED`:

1. Mark the model route as cooling down until `now + cooldown_seconds_on_429`.
2. Return `quota_exhausted` to C++ if no alternate route can run.
3. C++ closes the scan-cycle gate in enforce.

### TPM extension point

The state schema should include optional `tpm` fields, but v1 only enforces RPM/RPD unless token estimates are already reliable.

---

## 12. Error Codes

Normalize Python errors so C++ can apply deterministic policy.

| Error code | Meaning | C++ enforce behavior |
|---|---|---|
| `quota_exhausted` | Internal RPM/RPD bucket has no capacity or Gemini returned 429 and no route remains | Block order and close gate for cycle |
| `budget_exhausted` | C++ scan-cycle evaluation budget is exhausted | Block order and close gate for cycle |
| `component_error` | Sentiment or vision failed for non-quota reason | Block order, do not close gate by default |
| `model_resolution_error` | Resolver failed and no pinned/stale fallback is allowed | Block order, do not close gate by default |
| `timeout` | Gemini subprocess timed out | Block order, do not close gate by default |
| `invalid_input` | Bad payload from C++ to Python | Block order, alert log |

Only `quota_exhausted` and `budget_exhausted` close the gate for the rest of the scan cycle.

---

## 13. Config Additions

Add these fields under `gemini_filter`:

```json
{
  "gemini_filter": {
    "result_cache_ttl_seconds": 1800,
    "sentiment_cache_ttl_seconds": 3600,
    "sentiment_cache_max_stale_seconds": 21600,
    "model_resolution_ttl_seconds": 3600,
    "model_resolution_max_stale_seconds": 86400,
    "close_gate_on_budget_exhausted": true,
    "close_gate_on_quota_exhausted": true,
    "model_routing": {
      "enabled": true,
      "sentiment": {
        "candidates": ["gemini-2.5-flash-lite", "gemini-2.5-flash"],
        "gemini_2_5_thinking_budget": 0,
        "gemini_3_thinking_level": "minimal"
      },
      "vision": {
        "candidates": ["gemini-3.1-flash-lite", "gemini-3.1-pro-preview"],
        "gemini_2_5_thinking_budget": 0,
        "gemini_3_thinking_level": "low",
        "pro_escalation_enabled": true,
        "pro_escalation_min_score": 0.45,
        "pro_escalation_max_score": 0.65
      }
    },
    "quota": {
      "enabled": true,
      "safety_factor": 0.70,
      "cooldown_seconds_on_429": 300,
      "models": {}
    }
  }
}
```

For production, `quota.models` must be explicitly filled from the AI Studio limits for the active project.

---

## 14. Logging

Required new log lines:

| Event | Level | Example |
|---|---|---|
| Result cache hit | Info | `gemini result cache hit symbol=BLUAIUSDT tf=1h direction=Long decision=Block age_s=120` |
| Sentiment cache hit | Subprocess | `gemini.py \| sentiment cache hit symbol=BLUAIUSDT direction=Long age_s=900` |
| Model resolution cache hit | Subprocess | `gemini.py \| model resolution cache hit mode=latest_flash model=gemini-3.1-flash-lite age_s=500` |
| Quota reserve | Debug/Subprocess | `gemini.py \| quota reserved model=gemini-2.5-flash-lite component=sentiment rpm_tokens=12.4 rpd_used=41/700` |
| Quota exhausted | Warning | `gemini quota exhausted model=gemini-3.1-pro-preview component=vision retry_after_s=180` |
| Gate closed | Warning | `gemini gate closed for cycle reason="quota_exhausted" first_symbol=BLUAIUSDT first_tf=1h policy=fail_closed` |
| Gate summary | Info | `gemini gate cycle summary closed=true reason="quota_exhausted" skipped_items=842` |

Avoid logging one warning per skipped work item after the gate is closed.

---

## 15. Implementation Plan

### Phase 1 - C++ gate closure and final result cache

1. Add `GeminiCycleGate` to `SignalEngine`.
2. Reset it at the start of `runScanCycle()`.
3. Skip `processItem()` early when enforce gate is closed.
4. Replace repeated budget-exhausted logs with one `gemini gate closed for cycle` log.
5. Add in-memory final result cache keyed by chart fingerprint.
6. Add tests that candidate signals are not logged/evaluated after gate closure.

### Phase 2 - Python sentiment and model-resolution caches

1. Add file-backed cache helpers under `tools/gemini_filter/cache.py`.
2. Cache sentiment with TTL and optional stale fallback.
3. Cache model resolution with 1h TTL and stale-while-revalidate.
4. Return cache metadata in the JSON result for observability.
5. Add Python unit tests using fake clock and fake Gemini client.

### Phase 3 - Model routing

1. Extend input JSON with `model_routing`.
2. Select sentiment and vision routes independently.
3. Apply low-latency thinking config based on model family:
   - Gemini 2.5: `thinking_budget=0` for Flash/Flash-Lite.
   - Gemini 3: `thinking_level=minimal` or `low`.
4. Add optional vision Pro escalation only for ambiguous Flash scores.

### Phase 4 - Python quota manager

1. Add file-backed quota state and lock.
2. Enforce RPM token bucket per model.
3. Enforce RPD Pacific-day counter per model.
4. Mark model cooldown after 429/RESOURCE_EXHAUSTED.
5. Return normalized `quota_exhausted` when no route can run.
6. Add tests for RPM exhaustion, RPD exhaustion, cooldown, and alternate model route fallback.

---

## 16. Test Plan

### C++ tests

| Test | Expected |
|---|---|
| Gate closes on C++ budget exhausted in enforce | First exhaustion logs close; later `processItem()` skips before `strategy.evaluate()` |
| Gate does not close on ordinary Gemini model `Block` | Later work items can still evaluate |
| Gate does not close on non-quota component error | Later work items can still evaluate |
| Gate closes on `quota_exhausted` error code | Later work items skip before strategy evaluation |
| Final result cache hit | Gemini port not called again for same key |
| Chart fingerprint changes | Cache miss and Gemini evaluates again |
| Shadow mode budget exhausted | Does not close pre-strategy gate unless explicitly configured |

### Python tests

| Test | Expected |
|---|---|
| Sentiment cache hit inside TTL | No Gemini sentiment call |
| Sentiment stale fallback on quota exhaustion | Returns stale sentiment with metadata |
| Model resolution cache hit inside 1h | No `models.list()` call |
| Model resolution stale fallback | Uses stale or pinned model according to config |
| RPM bucket exhaustion | Returns `quota_exhausted` with retry-after |
| RPD Pacific reset | Counter resets at Pacific midnight |
| 429 cooldown | Model route is skipped until cooldown expires |
| Alternate route | Falls back from exhausted Pro to Flash when configured and available |

---

## 17. Operational Guidance

1. Fill `quota.models` from AI Studio for the exact Google Cloud project.
2. Keep sentiment on Flash/Lite by default.
3. Keep Pro Preview disabled or escalation-only for live vision.
4. Set `safety_factor` between `0.5` and `0.8` until logs prove stable.
5. Treat API keys as credentials, not quota pools, unless they are intentionally mapped to separate Google Cloud projects and configured as separate pools.
6. For backtests or large offline scoring, use Batch API separately from the live gate.

---

## 18. Acceptance Criteria

1. In `enforce`, after budget/quota exhaustion, the log contains exactly one `gemini gate closed for cycle` line per scan cycle.
2. After gate closure, no further strategy candidate logs are emitted in that cycle.
3. Repeated signal for the same symbol/direction/timeframe/chart fingerprint hits final result cache.
4. Repeated sentiment for the same asset/direction inside TTL hits sentiment cache.
5. `models.list()` is not called more than once per model-resolution key per hour.
6. The quota manager blocks before Gemini calls when internal RPM/RPD buckets are empty.
7. Gemini `429` causes model cooldown and normalized `quota_exhausted` if no alternate route is available.
8. No live order is placed without a fresh or cached Gemini `Allow` when `mode="enforce"`.
