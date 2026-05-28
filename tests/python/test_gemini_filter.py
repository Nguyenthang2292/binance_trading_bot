from __future__ import annotations

from typing import Any

import pytest

from tools.gemini_filter.gemini_filter import _validate_input


def _payload() -> dict[str, Any]:
    return {
        "eval_id": "eval-1",
        "symbol": "BTCUSDT",
        "direction": "Long",
        "primary_tf": "1h",
        "klines": {
            "1h": [
                {
                    "open_time": 1,
                    "open": "1.0",
                    "high": "1.5",
                    "low": "0.5",
                    "close": "1.2",
                    "volume": "10",
                }
            ]
        },
        "runtime_dir": "tmp/runtime",
        "sentiment_model": "s",
        "vision_model": "v",
        "sentiment_weight": 0.5,
        "vision_weight": 0.5,
        "confidence_threshold": 0.6,
    }


def test_validate_input_rejects_non_object_klines() -> None:
    payload = _payload()
    payload["klines"] = []
    with pytest.raises(RuntimeError, match="klines must be an object"):
        _validate_input(payload)


def test_validate_input_rejects_missing_kline_fields() -> None:
    payload = _payload()
    payload["klines"]["1h"] = [{"open_time": 1}]
    with pytest.raises(RuntimeError, match="missing fields"):
        _validate_input(payload)


def test_validate_input_rejects_invalid_extra_tfs_type() -> None:
    payload = _payload()
    payload["extra_tfs"] = "15m"
    with pytest.raises(RuntimeError, match="extra_tfs must be a list"):
        _validate_input(payload)
