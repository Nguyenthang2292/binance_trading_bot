from __future__ import annotations

import json
import math
import sys
from pathlib import Path
from typing import Any
from unittest.mock import MagicMock, patch

import pytest

# ── Import the module under test ────────────────────────────────────────────
from tools.backtest_range_proposer import main as brp


# ── Fixtures ─────────────────────────────────────────────────────────────────

TUNABLE_PARAMS = ["ma_short", "ma_long", "atr_period", "sl_multiplier", "tp_multiplier"]

VALID_DEFAULT_RANGES: list[dict[str, Any]] = [
    {"name": "ma_short",      "min": 10,  "max": 80,  "step": 5,    "is_integer": True},
    {"name": "ma_long",       "min": 50,  "max": 250, "step": 10,   "is_integer": True},
    {"name": "atr_period",    "min": 7,   "max": 21,  "step": 1,    "is_integer": True},
    {"name": "sl_multiplier", "min": 1.0, "max": 3.0, "step": 0.25, "is_integer": False},
    {"name": "tp_multiplier", "min": 2.0, "max": 5.0, "step": 0.25, "is_integer": False},
]

VALID_INPUT: dict[str, Any] = {
    "eval_id": "test-eval-001",
    "symbol": "BTCUSDT",
    "interval": "4h",
    "strategy_id": "golden_crossover",
    "tunable_params": TUNABLE_PARAMS,
    "current_values": {
        "ma_short": 50, "ma_long": 200, "atr_period": 14,
        "sl_multiplier": 1.5, "tp_multiplier": 3.0,
    },
    "prompt_context_aggregates": {
        "ret_30d_pct": 12.5,
        "atr_pct_current": 2.3,
        "avg_volume_usd": 850_000_000,
        "trend_direction": "up",
        "realized_vol_30d": 0.42,
        "num_candles": 500,
    },
    "default_ranges": VALID_DEFAULT_RANGES,
    "model": "gemini-2.0-flash",
}

VALID_GEMINI_RESPONSE_RANGES = [
    {"name": "ma_short",      "min": 20,  "max": 60,  "step": 5,    "is_integer": True},
    {"name": "ma_long",       "min": 100, "max": 220, "step": 10,   "is_integer": True},
    {"name": "atr_period",    "min": 10,  "max": 18,  "step": 1,    "is_integer": True},
    {"name": "sl_multiplier", "min": 1.25,"max": 2.5, "step": 0.25, "is_integer": False},
    {"name": "tp_multiplier", "min": 2.5, "max": 4.5, "step": 0.25, "is_integer": False},
]


def _make_input_file(tmp_path: Path, data: dict[str, Any] | None = None) -> Path:
    p = tmp_path / "input.json"
    p.write_text(json.dumps(data or VALID_INPUT), encoding="utf-8")
    return p


# ── Module importable ────────────────────────────────────────────────────────

def test_module_importable() -> None:
    assert hasattr(brp, "main")
    assert hasattr(brp, "_validate_input")
    assert hasattr(brp, "_validate_output_ranges")
    assert hasattr(brp, "_build_prompt")
    assert hasattr(brp, "_parse_gemini_response")


# ── Input validation ─────────────────────────────────────────────────────────

def test_validate_input_ok() -> None:
    brp._validate_input(VALID_INPUT)  # must not raise


@pytest.mark.parametrize("missing_field", brp.REQUIRED_FIELDS)
def test_validate_input_missing_field(missing_field: str) -> None:
    bad = {k: v for k, v in VALID_INPUT.items() if k != missing_field}
    with pytest.raises(ValueError, match=missing_field):
        brp._validate_input(bad)


def test_validate_range_entry_min_gt_max() -> None:
    r = {"name": "x", "min": 5.0, "max": 1.0, "step": 1.0, "is_integer": False}
    with pytest.raises(ValueError, match="min.*max"):
        brp._validate_range_entry(r)


def test_validate_range_entry_step_zero() -> None:
    r = {"name": "x", "min": 1.0, "max": 5.0, "step": 0.0, "is_integer": False}
    with pytest.raises(ValueError, match="step"):
        brp._validate_range_entry(r)


def test_validate_range_entry_non_finite_value() -> None:
    r = {"name": "x", "min": float("inf"), "max": 5.0, "step": 1.0, "is_integer": False}
    with pytest.raises(ValueError, match="finite"):
        brp._validate_range_entry(r)


# ── Output validation ─────────────────────────────────────────────────────────

def test_validate_output_ranges_ok() -> None:
    brp._validate_output_ranges(VALID_GEMINI_RESPONSE_RANGES, TUNABLE_PARAMS)


def test_validate_output_ranges_unknown_param() -> None:
    bad_ranges = [{"name": "unknown_param", "min": 1.0, "max": 5.0, "step": 1.0, "is_integer": False}]
    with pytest.raises(ValueError, match="not in tunable_params"):
        brp._validate_output_ranges(bad_ranges, TUNABLE_PARAMS)


def test_validate_output_ranges_min_gt_max_rejected() -> None:
    bad_ranges = [{"name": "ma_short", "min": 80.0, "max": 10.0, "step": 5.0, "is_integer": True}]
    with pytest.raises(ValueError, match="min.*max"):
        brp._validate_output_ranges(bad_ranges, TUNABLE_PARAMS)


# ── Prompt isolation — prompt_context only ───────────────────────────────────

def test_prompt_does_not_contain_calibration_data() -> None:
    """Gemini prompt must only contain stats from prompt_context_aggregates, not raw klines."""
    prompt = brp._build_prompt(VALID_INPUT)
    # The prompt should reference aggregates (like ret_30d_pct)
    assert "ret_30d_pct" in prompt
    # It should NOT contain any calibration window raw prices or OOS data
    # (in practice, only pre-computed aggregates are forwarded)
    assert "calibration" not in prompt.lower()
    assert "out_of_sample" not in prompt.lower()


def test_prompt_contains_symbol_and_strategy() -> None:
    prompt = brp._build_prompt(VALID_INPUT)
    assert VALID_INPUT["symbol"] in prompt
    assert VALID_INPUT["strategy_id"] in prompt
    assert VALID_INPUT["interval"] in prompt


# ── _parse_gemini_response ────────────────────────────────────────────────────

def test_parse_valid_response() -> None:
    raw = json.dumps({"ranges": VALID_GEMINI_RESPONSE_RANGES, "notes": "looks good"})
    result = brp._parse_gemini_response(raw, TUNABLE_PARAMS)
    assert result["notes"] == "looks good"
    assert len(result["ranges"]) == len(VALID_GEMINI_RESPONSE_RANGES)


def test_parse_malformed_json_raises() -> None:
    with pytest.raises(ValueError, match="invalid JSON"):
        brp._parse_gemini_response("{bad json}", TUNABLE_PARAMS)


def test_parse_missing_ranges_field_raises() -> None:
    raw = json.dumps({"notes": "ok"})
    with pytest.raises(ValueError, match="missing 'ranges'"):
        brp._parse_gemini_response(raw, TUNABLE_PARAMS)


def test_parse_unknown_param_name_raises() -> None:
    ranges = [{"name": "hack_param", "min": 1.0, "max": 5.0, "step": 1.0, "is_integer": False}]
    raw = json.dumps({"ranges": ranges, "notes": ""})
    with pytest.raises(ValueError, match="not in tunable_params"):
        brp._parse_gemini_response(raw, TUNABLE_PARAMS)


def test_parse_step_negative_raises() -> None:
    ranges = [{"name": "ma_short", "min": 10.0, "max": 80.0, "step": -1.0, "is_integer": True}]
    raw = json.dumps({"ranges": ranges, "notes": ""})
    with pytest.raises(ValueError, match="step"):
        brp._parse_gemini_response(raw, TUNABLE_PARAMS)


# ── main() integration (mocked Gemini) ───────────────────────────────────────

def _mock_call_gemini(_prompt: str, _model: str) -> str:
    return json.dumps({"ranges": VALID_GEMINI_RESPONSE_RANGES, "notes": "mocked"})


def test_main_success_with_output_file(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    input_file = _make_input_file(tmp_path)
    output_file = tmp_path / "output.json"
    monkeypatch.setattr(sys, "argv", ["main.py", str(input_file), str(output_file)])
    monkeypatch.setattr(brp, "_call_gemini", _mock_call_gemini)

    rc = brp.main()
    assert rc == 0
    result = json.loads(output_file.read_text())
    assert "ranges" in result
    assert result["eval_id"] == "test-eval-001"
    assert len(result["ranges"]) == 5


def test_main_success_prints_to_stdout(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    input_file = _make_input_file(tmp_path)
    monkeypatch.setattr(sys, "argv", ["main.py", str(input_file)])
    monkeypatch.setattr(brp, "_call_gemini", _mock_call_gemini)

    rc = brp.main()
    assert rc == 0
    out = capsys.readouterr().out.strip()
    result = json.loads(out)
    assert result["eval_id"] == "test-eval-001"


def test_main_missing_argv_returns_error(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(sys, "argv", ["main.py"])
    rc = brp.main()
    assert rc == 1


def test_main_invalid_input_json_returns_error(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    bad_file = tmp_path / "bad.json"
    bad_file.write_text("not json", encoding="utf-8")
    output_file = tmp_path / "out.json"
    monkeypatch.setattr(sys, "argv", ["main.py", str(bad_file), str(output_file)])
    rc = brp.main()
    assert rc == 1
    result = json.loads(output_file.read_text())
    assert result["error"] is True
    assert result["error_code"] == "invalid_input"


def test_main_gemini_error_returns_error(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    input_file = _make_input_file(tmp_path)
    output_file = tmp_path / "out.json"
    monkeypatch.setattr(sys, "argv", ["main.py", str(input_file), str(output_file)])
    monkeypatch.setattr(brp, "_call_gemini", lambda *_: (_ for _ in ()).throw(RuntimeError("api down")))

    rc = brp.main()
    assert rc == 1
    result = json.loads(output_file.read_text())
    assert result["error"] is True
    assert result["error_code"] == "gemini_error"


def test_main_invalid_gemini_response_returns_error(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    input_file = _make_input_file(tmp_path)
    output_file = tmp_path / "out.json"
    monkeypatch.setattr(sys, "argv", ["main.py", str(input_file), str(output_file)])
    # Return JSON but with unknown param
    bad_ranges = [{"name": "nonexistent_param", "min": 1.0, "max": 2.0, "step": 1.0, "is_integer": False}]
    monkeypatch.setattr(brp, "_call_gemini", lambda *_: json.dumps({"ranges": bad_ranges, "notes": ""}))

    rc = brp.main()
    assert rc == 1
    result = json.loads(output_file.read_text())
    assert result["error"] is True
    assert result["error_code"] == "invalid_response"


def test_main_schema_round_trip(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Input and output schema round-trip integrity."""
    input_file = _make_input_file(tmp_path)
    output_file = tmp_path / "out.json"
    monkeypatch.setattr(sys, "argv", ["main.py", str(input_file), str(output_file)])
    monkeypatch.setattr(brp, "_call_gemini", _mock_call_gemini)

    brp.main()
    result = json.loads(output_file.read_text())
    # All returned params must be in tunable_params
    for r in result["ranges"]:
        assert r["name"] in TUNABLE_PARAMS
        assert math.isfinite(r["min"])
        assert math.isfinite(r["max"])
        assert r["min"] <= r["max"]
        assert r["step"] > 0
