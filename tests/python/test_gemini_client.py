from __future__ import annotations

import pytest

from tools.gemini_filter.gemini_client import parse_score_text


def test_parse_score_text_valid() -> None:
    parsed = parse_score_text('{"score": 0.75, "analysis": "ok"}')
    assert parsed["score"] == 0.75
    assert parsed["analysis"] == "ok"


def test_parse_score_text_invalid_raises() -> None:
    with pytest.raises(Exception):
        parse_score_text('{"score": 5, "analysis": "bad"}')

