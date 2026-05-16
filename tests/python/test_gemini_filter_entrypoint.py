from __future__ import annotations

import json
from pathlib import Path

import pytest

from tools.gemini_filter import gemini_filter


def test_main_usage_error_when_missing_argument(monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]) -> None:
    monkeypatch.setattr(gemini_filter.sys, "argv", ["gemini_filter.py"])
    rc = gemini_filter.main()
    out = capsys.readouterr().out.strip()
    parsed = json.loads(out)
    assert rc == 0
    assert parsed["error_code"] == "usage_error"


def test_main_invalid_input_returns_block(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    bad_input = tmp_path / "input.json"
    bad_input.write_text("[]", encoding="utf-8")
    monkeypatch.setattr(gemini_filter.sys, "argv", ["gemini_filter.py", str(bad_input)])
    rc = gemini_filter.main()
    out = capsys.readouterr().out.strip()
    parsed = json.loads(out)
    assert rc == 0
    assert parsed["error_code"] == "invalid_input"


def test_main_success_path(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    good_input = tmp_path / "input.json"
    good_input.write_text(
        json.dumps(
            {
                "eval_id": "e1",
                "symbol": "BTCUSDT",
                "direction": "Long",
                "primary_tf": "1h",
                "klines": {"1h": [{"open_time": 1, "close_time": 2, "open": 1, "high": 2, "low": 1, "close": 2, "volume": 1}]},
                "runtime_dir": str(tmp_path),
                "sentiment_model": "s",
                "vision_model": "v",
                "sentiment_weight": 0.5,
                "vision_weight": 0.5,
                "confidence_threshold": 0.6,
            }
        ),
        encoding="utf-8",
    )

    monkeypatch.setattr(gemini_filter.sys, "argv", ["gemini_filter.py", str(good_input)])
    monkeypatch.setattr(gemini_filter, "GeminiKeyManager", lambda: object())
    monkeypatch.setattr(
        gemini_filter,
        "analyze",
        lambda _payload, _key_manager: {
            "eval_id": "e1",
            "decision": "Allow",
            "confidence": 0.9,
            "sentiment_score": 0.9,
            "vision_score": 0.9,
            "sentiment_analysis": "ok",
            "vision_analysis": "ok",
            "reason": "ok",
            "error_code": None,
            "error": None,
            "latency_ms": 10,
        },
    )

    rc = gemini_filter.main()
    out = capsys.readouterr().out.strip()
    parsed = json.loads(out)
    assert rc == 0
    assert parsed["decision"] == "Allow"

