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
import os
import sys
from pathlib import Path
from typing import Any

from dotenv import load_dotenv

LOGGER = logging.getLogger("backtest_range_proposer")

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


def _validate_input(data: dict[str, Any]) -> None:
    for field in REQUIRED_FIELDS:
        if field not in data:
            raise ValueError(f"missing required field: {field}")

    if not isinstance(data["tunable_params"], list):
        raise ValueError("tunable_params must be a list")
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


def _validate_output_ranges(ranges: list[Any], tunable_params: list[str]) -> None:
    """Validate ranges returned by Gemini."""
    if not isinstance(ranges, list):
        raise ValueError("output 'ranges' must be a list")
    for r in ranges:
        _validate_range_entry(r, context="output range entry")
        if r["name"] not in tunable_params:
            raise ValueError(
                f"output range contains param '{r['name']}' not in tunable_params: {tunable_params}"
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

def _call_gemini(prompt: str, model: str) -> str:
    """Call Gemini API and return the raw text response."""
    try:
        from google import genai  # type: ignore[import]
        from google.genai import types  # type: ignore[import]
    except ImportError as exc:
        raise RuntimeError("google-genai package not installed") from exc

    api_key = os.environ.get("GEMINI_API_KEY")
    if not api_key:
        raise RuntimeError(
            "GEMINI_API_KEY environment variable not set. "
            "See tools/backtest_range_proposer/README.md for setup instructions."
        )

    client = genai.Client(api_key=api_key)
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


# ── Parse & validate Gemini response ────────────────────────────────────────

def _parse_gemini_response(raw: str, tunable_params: list[str]) -> dict[str, Any]:
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ValueError(f"Gemini returned invalid JSON: {exc}") from exc

    if not isinstance(parsed, dict):
        raise ValueError("Gemini response must be a JSON object")

    if "ranges" not in parsed:
        raise ValueError("Gemini response missing 'ranges' field")

    ranges = parsed["ranges"]
    _validate_output_ranges(ranges, tunable_params)

    notes = parsed.get("notes", "")
    if not isinstance(notes, str):
        notes = str(notes)

    return {"ranges": ranges, "notes": notes}


# ── Main ─────────────────────────────────────────────────────────────────────

def main() -> int:
    logging.basicConfig(
        level=logging.INFO,
        format="%(levelname).1s %(name)s | %(message)s",
        stream=sys.stderr,
    )
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
    except Exception as exc:  # noqa: BLE001
        LOGGER.exception("invalid input eval_id=%s", eval_id)
        err = _error_result(eval_id, "invalid input", "invalid_input", exc)
        _write_output(err, output_path)
        return 1

    model = payload.get("model", "gemini-2.0-flash")
    tunable_params: list[str] = payload["tunable_params"]

    # ── Build prompt ───────────────────────────────────────────────────────
    prompt = _build_prompt(payload)
    LOGGER.info("prompt built len=%d eval_id=%s", len(prompt), eval_id)

    # ── Call Gemini ────────────────────────────────────────────────────────
    try:
        raw_response = _call_gemini(prompt, model)
        LOGGER.info("gemini responded eval_id=%s response_len=%d", eval_id, len(raw_response))
    except Exception as exc:  # noqa: BLE001
        LOGGER.exception("gemini call failed eval_id=%s", eval_id)
        err = _error_result(eval_id, "gemini call failed", "gemini_error", exc)
        _write_output(err, output_path)
        return 1

    # ── Parse & validate response ──────────────────────────────────────────
    try:
        result_data = _parse_gemini_response(raw_response, tunable_params)
    except Exception as exc:  # noqa: BLE001
        LOGGER.exception("gemini response validation failed eval_id=%s", eval_id)
        err = _error_result(eval_id, "invalid gemini response", "invalid_response", exc)
        _write_output(err, output_path)
        return 1

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
        output_path.write_text(serialized, encoding="utf-8")
    else:
        print(serialized)


if __name__ == "__main__":
    raise SystemExit(main())
