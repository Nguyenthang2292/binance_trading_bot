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


from unittest.mock import MagicMock, patch, call
from google.genai import types


def _make_fake_response(text: str) -> MagicMock:
    resp = MagicMock()
    resp.text = text
    return resp


def test_generate_json_score_without_search_uses_json_schema() -> None:
    """Config must use response_mime_type and response_schema, no tools."""
    fake_client = MagicMock()
    fake_client.models.generate_content.return_value = _make_fake_response(
        '{"score": 0.8, "analysis": "good"}'
    )
    from tools.gemini_filter.gemini_client import generate_json_score

    result = generate_json_score(
        client=fake_client,
        model="gemini-2.0-flash",
        contents="prompt",
        use_google_search=False,
    )
    assert result["score"] == 0.8
    config_used = fake_client.models.generate_content.call_args.kwargs["config"]
    assert config_used.response_mime_type == "application/json"
    assert config_used.tools is None or config_used.tools == []


def test_generate_json_score_with_search_omits_json_schema() -> None:
    """When search is enabled, config must NOT set response_mime_type=json (incompatible)."""
    fake_client = MagicMock()
    fake_client.models.generate_content.return_value = _make_fake_response(
        '{"score": 0.6, "analysis": "ok"}'
    )
    from tools.gemini_filter.gemini_client import generate_json_score

    result = generate_json_score(
        client=fake_client,
        model="gemini-2.0-flash",
        contents="prompt",
        use_google_search=True,
    )
    assert result["score"] == 0.6
    config_used = fake_client.models.generate_content.call_args.kwargs["config"]
    # JSON schema mode must be absent when grounding is active
    assert getattr(config_used, "response_mime_type", None) != "application/json"
    # Search tool must be present
    tools = getattr(config_used, "tools", None) or []
    assert len(tools) == 1


def test_generate_json_score_config_built_in_single_call() -> None:
    """Config object must not be mutated after construction (no post-init attribute assignment)."""
    configs_captured: list = []

    def fake_generate(**kwargs):  # type: ignore[override]
        configs_captured.append(kwargs["config"])
        resp = MagicMock()
        resp.text = '{"score": 0.5, "analysis": "x"}'
        return resp

    fake_client = MagicMock()
    fake_client.models.generate_content.side_effect = fake_generate
    from tools.gemini_filter.gemini_client import generate_json_score

    generate_json_score(
        client=fake_client,
        model="gemini-2.0-flash",
        contents="prompt",
        use_google_search=True,
    )
    assert len(configs_captured) == 1
    # The config passed must already have tools set (not None), proving no post-construction mutation
    cfg = configs_captured[0]
    assert (getattr(cfg, "tools", None) or [])  # non-empty tools list

