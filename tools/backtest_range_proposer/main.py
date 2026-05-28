#!/usr/bin/env python3
"""Backtest Range Proposer — entry point.

Contract:
  argv[1] = path to input JSON file
  argv[2] = path to write output JSON file  (optional; prints to stdout if omitted)
  exit 0  = success; output JSON written
  exit 1  = internal error (structured error on stderr)

Input JSON schema:
  eval_id        : str
  symbol         : str
  interval       : str
  strategy_id    : str
  tunable_params : list[str]
  current_values : dict[str, float]
  prompt_context_aggregates : {
    ret_30d_pct   : float,
    atr_pct_current : float,
    avg_volume_usd : float,
    trend_direction : "up" | "down" | "flat",
    ...
  }
  default_ranges : list[{name, min, max, step, is_integer}]
  model          : str   (optional, defaults to gemini-2.0-flash)

Output JSON schema:
  eval_id : str
  ranges  : list[{name, min, max, step, is_integer}]
  notes   : str
"""
from __future__ import annotations

import json
import logging
import math
import sys
from pathlib import Path
from typing import Any

from dotenv import load_dotenv

LOGGER = logging.getLogger("backtest_range_proposer")
GEMINI_MIN_ATTEMPT_DEADLINE_MS = 1_000
GEMINI_TIMEOUT_HEADROOM_SECONDS = 2

# ── Output helpers ─────────────────────────────────────────────────────────

def _error_result(eval_id: str, reason: str, error_code: str, exc: Exception | None) -> dict[str, Any]:
    return {
        "eval_id": eval_id,
        "error": True,
        "error_code": error_code,
        "reason": reason,
        "details": str(exc) if exc else None,
    }


# ── Input validation ────────────────────────────────────────────────────────

REQUIRED_FIELDS = (
    "eval_id",
    "symbol",
    "interval",
    "strategy_id",
    "tunable_params",
    "current_values",
    "prompt_context_aggregates",
    "default_ranges",
)


def _validate_prompt_identity_field(value: Any, field_name: str) -> None:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{field_name} must be a non-empty string")
    if "\n" in value or "\r" in value:
        raise ValueError(f"{field_name} must not contain newlines")
    if "#" in value:
        raise ValueError(f"{field_name} must not contain markdown markers ('#')")


def _validate_input(data: dict[str, Any]) -> None:
    for field in REQUIRED_FIELDS:
        if field not in data:
            raise ValueError(f"missing required field: {field}")

    _validate_prompt_identity_field(data["eval_id"], "eval_id")
    _validate_prompt_identity_field(data["symbol"], "symbol")
    _validate_prompt_identity_field(data["interval"], "interval")
    _validate_prompt_identity_field(data["strategy_id"], "strategy_id")

    if not isinstance(data["tunable_params"], list):
        raise ValueError("tunable_params must be a list")
    if not all(isinstance(param, str) and param.strip() for param in data["tunable_params"]):
        raise ValueError("tunable_params must contain only non-empty strings")
    if not isinstance(data["current_values"], dict):
        raise ValueError("current_values must be an object")
    if not isinstance(data["prompt_context_aggregates"], dict):
        raise ValueError("prompt_context_aggregates must be an object")
    if not isinstance(data["default_ranges"], list):
        raise ValueError("default_ranges must be a list")
    for r in data["default_ranges"]:
        _validate_range_entry(r, context="default_ranges entry")


def _validate_range_entry(r: Any, context: str = "range entry") -> None:
    if not isinstance(r, dict):
        raise ValueError(f"{context}: must be an object")
    for key in ("name", "min", "max", "step", "is_integer"):
        if key not in r:
            raise ValueError(f"{context}: missing field '{key}'")
    if not isinstance(r["name"], str) or not r["name"].strip():
        raise ValueError(f"{context}: 'name' must be a non-empty string")
    for key in ("min", "max", "step"):
        v = r[key]
        if not isinstance(v, (int, float)) or not math.isfinite(v):
            raise ValueError(f"{context}: '{key}' must be a finite number")
    if r["min"] > r["max"]:
        raise ValueError(f"{context}: min ({r['min']}) > max ({r['max']}) for param '{r['name']}'")
    if r["step"] <= 0:
        raise ValueError(f"{context}: step must be > 0 for param '{r['name']}'")


def _is_whole_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and math.isfinite(value) and float(value).is_integer()


def _validate_output_ranges(
    ranges: list[Any],
    tunable_params: list[str],
    *,
    default_ranges: list[dict[str, Any]] | None = None,
    current_values: dict[str, Any] | None = None,
    constraints: list[dict[str, Any]] | None = None,
) -> None:
    """Validate ranges returned by Gemini."""
    if not isinstance(ranges, list):
        raise ValueError("output 'ranges' must be a list")
    ranges_by_name: dict[str, dict[str, Any]] = {}
    for r in ranges:
        _validate_range_entry(r, context="output range entry")
        if r["name"] not in tunable_params:
            raise ValueError(
                f"output range contains param '{r['name']}' not in tunable_params: {tunable_params}"
            )
        if r["is_integer"]:
            for key in ("min", "max", "step"):
                if not _is_whole_number(r[key]):
                    raise ValueError(
                        f"output range entry: integer param '{r['name']}' has non-integer {key}={r[key]!r}"
                    )
        ranges_by_name[r["name"]] = r

    if default_ranges:
        default_is_integer_by_name = {str(r["name"]): bool(r["is_integer"]) for r in default_ranges}
        for name, r in ranges_by_name.items():
            if name in default_is_integer_by_name and bool(r["is_integer"]) != default_is_integer_by_name[name]:
                raise ValueError(
                    f"output range changed is_integer for '{name}' "
                    f"(default={default_is_integer_by_name[name]}, output={r['is_integer']})"
                )

    if current_values:
        for name, r in ranges_by_name.items():
            if name not in current_values:
                continue
            current = current_values[name]
            if not isinstance(current, (int, float)) or not math.isfinite(current):
                raise ValueError(f"current_values['{name}'] must be a finite number")
            if current < r["min"] or current > r["max"]:
                raise ValueError(
                    f"output range for '{name}' does not cover current value {current} "
                    f"(range=[{r['min']}, {r['max']}])"
                )

    if constraints:
        for idx, constraint in enumerate(constraints):
            if not isinstance(constraint, dict):
                raise ValueError(f"constraints[{idx}] must be an object")
            left = constraint.get("left")
            right = constraint.get("right")
            kind = constraint.get("kind")
            if not isinstance(left, str) or not isinstance(right, str):
                raise ValueError(f"constraints[{idx}] must contain string fields 'left' and 'right'")
            if left not in ranges_by_name or right not in ranges_by_name:
                raise ValueError(
                    f"constraint references unknown range(s): left={left!r}, right={right!r}"
                )
            if kind not in ("less_than", "less_than_or_equal"):
                raise ValueError(f"unsupported constraint kind {kind!r} for pair ({left}, {right})")

            left_min = ranges_by_name[left]["min"]
            right_max = ranges_by_name[right]["max"]
            compatible = left_min < right_max if kind == "less_than" else left_min <= right_max
            if not compatible:
                op = "<" if kind == "less_than" else "<="
                raise ValueError(
                    f"constraint {left} {op} {right} is incompatible with proposed ranges: "
                    f"left.min={left_min}, right.max={right_max}"
                )


# ── Prompt building ─────────────────────────────────────────────────────────

def _build_prompt(data: dict[str, Any]) -> str:
    aggs = data["prompt_context_aggregates"]
    defaults = data["default_ranges"]
    current = data["current_values"]

    defaults_str = json.dumps(defaults, ensure_ascii=True)
    current_str = json.dumps(current, ensure_ascii=True)
    aggs_str = json.dumps(aggs, ensure_ascii=True, indent=2)

    # Optional sections — only emitted when the C++ side sends them.
    constraints = data.get("constraints", [])
    signal = data.get("signal", {})
    budget = data.get("budget", {})

    constraints_section = ""
    if constraints:
        constraints_str = json.dumps(constraints, ensure_ascii=True)
        constraints_section = f"\n## Parameter Constraints\n{constraints_str}\n"

    signal_section = ""
    if signal:
        direction = signal.get("direction", "unknown")
        signal_section = f"\n## Live Signal\nDirection: {direction}\n"

    budget_section = ""
    if budget:
        max_combos = budget.get("max_total_combos", "unknown")
        budget_section = (
            f"\n## Grid Budget\nmax_total_combos={max_combos} — "
            "keep ranges tight enough that the grid fits within this budget.\n"
        )

    prompt = f"""You are a quantitative trading assistant for a crypto futures bot.

Your task: propose optimized parameter SEARCH RANGES for a walk-forward backtest optimizer.

## Symbol
{data['symbol']} @ {data['interval']}

## Strategy
{data['strategy_id']}

## Tunable Parameters
{json.dumps(data['tunable_params'], ensure_ascii=True)}

## Current Live Values
{current_str}

## Default Search Ranges (fallback reference)
{defaults_str}
{constraints_section}{signal_section}{budget_section}
## Market Context (computed from recent closed candles ONLY — do not infer anything beyond this)
{aggs_str}

## Instructions
Based on the market context, propose tighter or wider search ranges that are likely to produce better out-of-sample Sortino ratios.

Rules:
1. Return ONLY the params listed in "Tunable Parameters".
2. Each range: min <= max, step > 0, all finite numbers.
3. For integer params (is_integer=true), min/max/step must be whole numbers.
4. Do NOT change the is_integer flag — keep as provided in default ranges.
5. Ranges should cover the current live values (i.e., current value is inside the range).
6. Keep ranges practical — avoid ranges so wide that grid search becomes infeasible.
7. If Parameter Constraints are listed, every proposed range must be compatible with them
   (e.g. if left < right is required, ensure your ranges allow at least one valid pair).
8. Respond with ONLY a JSON object with fields:
   - "ranges": array of {{name, min, max, step, is_integer}} objects
   - "notes": a short plain-text explanation of your reasoning (1-3 sentences)

Respond with valid JSON only, no markdown fences.
"""
    return prompt.strip()


# ── Gemini call ──────────────────────────────────────────────────────────────

def _call_gemini(prompt: str, model: str, http_timeout_ms: int = 30_000) -> str:
    """Call Gemini API and return the raw text response."""
    try:
        from tools.shared.gemini_key_manager import GeminiKeyManager
        from google.genai import types  # type: ignore[import]
    except ImportError as exc:
        raise RuntimeError("google-genai or tools.shared package not available") from exc

    total_budget_ms = max(1, int(http_timeout_ms))
    probe_manager = GeminiKeyManager(http_timeout_ms=total_budget_ms)
    key_count = max(1, probe_manager.key_count)
    per_attempt_timeout_ms = max(1, total_budget_ms // key_count)
    if per_attempt_timeout_ms < GEMINI_MIN_ATTEMPT_DEADLINE_MS:
        LOGGER.warning(
            "per-attempt timeout dropped below %d ms to honor total budget across keys "
            "(total_budget_ms=%d key_count=%d per_attempt_timeout_ms=%d)",
            GEMINI_MIN_ATTEMPT_DEADLINE_MS,
            total_budget_ms,
            key_count,
            per_attempt_timeout_ms,
        )
    if key_count <= 1:
        key_manager = probe_manager
    else:
        key_manager = GeminiKeyManager(http_timeout_ms=per_attempt_timeout_ms)
    LOGGER.info(
        "gemini key manager initialized key_count=%d key_sources=%s total_budget_ms=%d per_attempt_timeout_ms=%d",
        key_manager.key_count,
        ",".join(key_manager.key_names),
        total_budget_ms,
        per_attempt_timeout_ms,
    )

    def _do_call(client: Any, _key: Any) -> str:
        response = client.models.generate_content(
            model=model,
            contents=prompt,
            config=types.GenerateContentConfig(
                response_mime_type="application/json",
            ),
        )
        text = getattr(response, "text", None)
        if not isinstance(text, str) or not text.strip():
            raise RuntimeError("Gemini response contained no text")
        return text

    return key_manager.run_with_rotation(_do_call)


# ── Parse & validate Gemini response ────────────────────────────────────────

def _parse_gemini_response(
    raw: str,
    tunable_params: list[str],
    *,
    default_ranges: list[dict[str, Any]] | None = None,
    current_values: dict[str, Any] | None = None,
    constraints: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ValueError(f"Gemini returned invalid JSON: {exc}") from exc

    if not isinstance(parsed, dict):
        raise ValueError("Gemini response must be a JSON object")

    if "ranges" not in parsed:
        raise ValueError("Gemini response missing 'ranges' field")

    ranges = parsed["ranges"]
    _validate_output_ranges(
        ranges,
        tunable_params,
        default_ranges=default_ranges,
        current_values=current_values,
        constraints=constraints,
    )

    notes = parsed.get("notes", "")
    if not isinstance(notes, str):
        notes = str(notes)

    return {"ranges": ranges, "notes": notes}


def _derive_outer_timeout_seconds(payload: dict[str, Any]) -> int:
    budget = payload.get("budget")
    outer_timeout_raw: Any = 30
    if isinstance(budget, dict):
        outer_timeout_raw = budget.get("timeout_seconds", 30)

    if isinstance(outer_timeout_raw, bool):
        raise ValueError("budget.timeout_seconds must be a positive integer")
    if isinstance(outer_timeout_raw, int):
        outer_timeout_seconds = outer_timeout_raw
    elif isinstance(outer_timeout_raw, float):
        if not math.isfinite(outer_timeout_raw) or not outer_timeout_raw.is_integer():
            raise ValueError("budget.timeout_seconds must be a positive integer")
        outer_timeout_seconds = int(outer_timeout_raw)
    elif isinstance(outer_timeout_raw, str):
        if not outer_timeout_raw.strip().isdigit():
            raise ValueError("budget.timeout_seconds must be a positive integer")
        outer_timeout_seconds = int(outer_timeout_raw.strip())
    else:
        raise ValueError("budget.timeout_seconds must be a positive integer")

    if outer_timeout_seconds <= 0:
        raise ValueError("budget.timeout_seconds must be > 0")
    return outer_timeout_seconds


def _emit_error_and_return(
    eval_id: str,
    *,
    reason: str,
    error_code: str,
    exc: Exception | None,
    output_path: Path | None,
    log_message: str,
) -> int:
    if exc is None:
        LOGGER.error("%s eval_id=%s", log_message, eval_id)
    else:
        LOGGER.exception("%s eval_id=%s", log_message, eval_id)
    err = _error_result(eval_id, reason, error_code, exc)
    _write_output(err, output_path)
    return 1


def _configure_logging_once() -> None:
    root_logger = logging.getLogger()
    if root_logger.handlers:
        return
    logging.basicConfig(
        level=logging.INFO,
        format="%(levelname).1s %(name)s | %(message)s",
        stream=sys.stderr,
    )


# ── Main ─────────────────────────────────────────────────────────────────────

def main() -> int:
    load_dotenv()

    if len(sys.argv) < 2:
        print(
            json.dumps(
                _error_result("", "usage error", "usage_error", None)
            ),
            file=sys.stderr,
        )
        LOGGER.error(
            "Usage: python -m tools.backtest_range_proposer.main <input_json> [<output_json>]"
        )
        return 1

    input_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2]) if len(sys.argv) >= 3 else None
    eval_id = ""

    # ── Load & validate input ──────────────────────────────────────────────
    try:
        raw_input = input_path.read_text(encoding="utf-8-sig")
        payload: dict[str, Any] = json.loads(raw_input)
        if not isinstance(payload, dict):
            raise ValueError("input JSON must be an object")
        eval_id = str(payload.get("eval_id", ""))
        _validate_input(payload)
        LOGGER.info("request accepted eval_id=%s symbol=%s strategy=%s",
                    eval_id, payload.get("symbol"), payload.get("strategy_id"))
    except (OSError, json.JSONDecodeError, TypeError, ValueError) as exc:
        return _emit_error_and_return(
            eval_id,
            reason="invalid input",
            error_code="invalid_input",
            exc=exc,
            output_path=output_path,
            log_message="invalid input",
        )

    model = payload.get("model", "gemini-2.0-flash")
    try:
        outer_timeout_seconds = _derive_outer_timeout_seconds(payload)
    except ValueError as exc:
        return _emit_error_and_return(
            eval_id,
            reason="invalid input",
            error_code="invalid_input",
            exc=exc,
            output_path=output_path,
            log_message="invalid input budget timeout",
        )
    if outer_timeout_seconds <= GEMINI_TIMEOUT_HEADROOM_SECONDS:
        return _emit_error_and_return(
            eval_id,
            reason="invalid input",
            error_code="invalid_input",
            exc=ValueError(
                "budget.timeout_seconds must be greater than "
                f"GEMINI_TIMEOUT_HEADROOM_SECONDS ({GEMINI_TIMEOUT_HEADROOM_SECONDS})"
            ),
            output_path=output_path,
            log_message="invalid input budget timeout",
        )
    sdk_timeout_ms = max(
        GEMINI_MIN_ATTEMPT_DEADLINE_MS,
        (outer_timeout_seconds - GEMINI_TIMEOUT_HEADROOM_SECONDS) * 1_000,
    )
    if outer_timeout_seconds <= GEMINI_TIMEOUT_HEADROOM_SECONDS + 2:
        LOGGER.warning(
            "budget timeout_seconds=%s is low; sdk timeout budget=%d ms (minimum floor=%d ms)",
            outer_timeout_seconds,
            sdk_timeout_ms,
            GEMINI_MIN_ATTEMPT_DEADLINE_MS,
        )
    tunable_params: list[str] = payload["tunable_params"]

    # ── Build prompt ───────────────────────────────────────────────────────
    prompt = _build_prompt(payload)
    LOGGER.info("prompt built len=%d eval_id=%s", len(prompt), eval_id)

    # ── Call Gemini ────────────────────────────────────────────────────────
    try:
        raw_response = _call_gemini(prompt, model, http_timeout_ms=sdk_timeout_ms)
        LOGGER.info("gemini responded eval_id=%s response_len=%d", eval_id, len(raw_response))
    except Exception as exc:  # noqa: BLE001
        return _emit_error_and_return(
            eval_id,
            reason="gemini call failed",
            error_code="gemini_error",
            exc=exc,
            output_path=output_path,
            log_message="gemini call failed",
        )

    # ── Parse & validate response ──────────────────────────────────────────
    try:
        result_data = _parse_gemini_response(
            raw_response,
            tunable_params,
            default_ranges=payload.get("default_ranges"),
            current_values=payload.get("current_values"),
            constraints=payload.get("constraints"),
        )
    except (json.JSONDecodeError, TypeError, ValueError) as exc:
        return _emit_error_and_return(
            eval_id,
            reason="invalid gemini response",
            error_code="invalid_response",
            exc=exc,
            output_path=output_path,
            log_message="gemini response validation failed",
        )

    # ── Write output ───────────────────────────────────────────────────────
    output: dict[str, Any] = {
        "eval_id": eval_id,
        "ranges": result_data["ranges"],
        "notes": result_data["notes"],
    }
    LOGGER.info(
        "request completed eval_id=%s num_ranges=%d notes=%r",
        eval_id,
        len(output["ranges"]),
        output["notes"][:80],
    )
    _write_output(output, output_path)
    return 0


def _write_output(data: dict[str, Any], output_path: Path | None) -> None:
    serialized = json.dumps(data, ensure_ascii=True)
    if output_path is not None:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        temp_path = output_path.with_suffix(f"{output_path.suffix}.tmp")
        temp_path.write_text(serialized, encoding="utf-8")
        temp_path.replace(output_path)
    else:
        print(serialized)


if __name__ == "__main__":
    _configure_logging_once()
    raise SystemExit(main())
