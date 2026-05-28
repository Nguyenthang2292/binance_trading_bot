#!/usr/bin/env python3
from __future__ import annotations

import json
import logging
import sys
from pathlib import Path
from typing import Any, cast

from dotenv import load_dotenv

from .analyzer import analyze
from tools.shared.gemini_key_manager import GeminiKeyManager

LOGGER = logging.getLogger("gemini_filter")
_REQUIRED_KLINE_FIELDS = ("open_time", "open", "high", "low", "close", "volume")


def _block_result(eval_id: str, reason: str, error_code: str, error: str | None) -> dict[str, Any]:
    return {
        "eval_id": eval_id,
        "decision": "Block",
        "confidence": 0.0,
        "sentiment_score": 0.0,
        "vision_score": 0.0,
        "sentiment_analysis": "",
        "vision_analysis": "",
        "reason": reason,
        "error_code": error_code,
        "error": error,
        "latency_ms": 0,
    }


def _validate_input(data: dict[str, Any]) -> None:
    required_fields = (
        "eval_id",
        "symbol",
        "direction",
        "primary_tf",
        "klines",
        "runtime_dir",
        "sentiment_model",
        "vision_model",
        "sentiment_weight",
        "vision_weight",
        "confidence_threshold",
    )
    for field in required_fields:
        if field not in data:
            raise RuntimeError(f"missing input field: {field}")

    primary_tf = str(data.get("primary_tf", "")).strip()
    if not primary_tf:
        raise RuntimeError("invalid_input: primary_tf must be non-empty")

    klines_raw = data.get("klines")
    if not isinstance(klines_raw, dict):
        raise RuntimeError("invalid_input: klines must be an object keyed by timeframe")
    if primary_tf not in klines_raw:
        raise RuntimeError(f"invalid_input: klines missing primary timeframe {primary_tf}")

    for tf, series in klines_raw.items():
        if not isinstance(series, list):
            raise RuntimeError(f"invalid_input: klines[{tf}] must be a list")
        if tf == primary_tf and not series:
            raise RuntimeError(f"invalid_input: klines[{tf}] must be non-empty")
        for idx, row in enumerate(series):
            if not isinstance(row, dict):
                raise RuntimeError(f"invalid_input: klines[{tf}][{idx}] must be an object")
            missing = [name for name in _REQUIRED_KLINE_FIELDS if name not in row]
            if missing:
                raise RuntimeError(
                    f"invalid_input: klines[{tf}][{idx}] missing fields: {','.join(missing)}"
                )

    extra_tfs = data.get("extra_tfs")
    if extra_tfs is not None and not isinstance(extra_tfs, list):
        raise RuntimeError("invalid_input: extra_tfs must be a list when provided")


def main() -> int:
    logging.basicConfig(
        level=logging.INFO,
        format="%(levelname).1s %(name)s | %(message)s",
        stream=sys.stderr,
    )
    for noisy_logger in ("choreographer", "google_genai", "httpx", "kaleido"):
        logging.getLogger(noisy_logger).setLevel(logging.WARNING)
    load_dotenv()

    if len(sys.argv) != 2:
        print(json.dumps(_block_result("", "usage error", "usage_error", "python -m tools.gemini_filter.gemini_filter <input_file>")))
        # Exit 0 by contract: C++ parent reads structured JSON from stdout instead of shell status.
        return 0

    input_path = Path(sys.argv[1])
    eval_id = ""
    try:
        payload = json.loads(input_path.read_text(encoding="utf-8-sig"))
        if not isinstance(payload, dict):
            raise RuntimeError("input JSON must be an object")
        payload_typed = cast(dict[str, Any], payload)
        eval_id = str(payload_typed.get("eval_id", ""))
        _validate_input(payload_typed)
        LOGGER.info("request accepted eval_id=%s input=%s", eval_id, input_path)
    except Exception as exc:  # noqa: BLE001
        LOGGER.exception("invalid input eval_id=%s", eval_id)
        print(json.dumps(_block_result(eval_id, "invalid input", "invalid_input", str(exc))))
        return 0

    try:
        key_manager = GeminiKeyManager()
        LOGGER.info(
            "gemini key manager initialized eval_id=%s key_count=%d key_sources=%s",
            eval_id,
            int(getattr(key_manager, "key_count", 0)),
            ",".join(getattr(key_manager, "key_names", ())),
        )
    except Exception as exc:  # noqa: BLE001
        LOGGER.exception("gemini key initialization failed eval_id=%s", eval_id)
        print(json.dumps(_block_result(eval_id, "No Gemini API key found", "no_api_key", str(exc))))
        return 0

    try:
        result = analyze(payload_typed, key_manager)
    except Exception as exc:  # noqa: BLE001
        LOGGER.exception("unexpected analyzer failure eval_id=%s", eval_id)
        result = _block_result(eval_id, "Unexpected Gemini filter error", "analyzer_exception", str(exc))

    LOGGER.info("request completed eval_id=%s decision=%s", eval_id, result.get("decision", ""))
    # Keep stdout ASCII-only so the C++ parent can parse JSON reliably from a Windows pipe.
    print(json.dumps(result, ensure_ascii=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
