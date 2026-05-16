from __future__ import annotations

import pytest
from unittest.mock import MagicMock
from google.genai import errors as genai_errors

from tools.gemini_filter.key_manager import GeminiKeyManager, _is_retryable_key_error, _split_key_list, _HTTP_TIMEOUT_MS


def test_split_key_list_supports_multiple_delimiters() -> None:
    parsed = _split_key_list("k1;k2,k3\nk4")
    assert parsed == ["k1", "k2", "k3", "k4"]


def _api_error(code: int) -> Exception:
    exc = MagicMock(spec=genai_errors.APIError)
    exc.code = code
    exc.status_code = None
    return exc


def test_403_is_not_retryable() -> None:
    assert not _is_retryable_key_error(_api_error(403))


def test_401_is_retryable() -> None:
    assert _is_retryable_key_error(_api_error(401))


def test_429_is_retryable() -> None:
    assert _is_retryable_key_error(_api_error(429))


def test_500_is_retryable() -> None:
    assert _is_retryable_key_error(_api_error(500))


def test_502_is_retryable() -> None:
    assert _is_retryable_key_error(_api_error(502))


def test_503_is_retryable() -> None:
    assert _is_retryable_key_error(_api_error(503))


def test_504_is_retryable() -> None:
    assert _is_retryable_key_error(_api_error(504))


def test_client_constructed_with_http_timeout(monkeypatch: pytest.MonkeyPatch) -> None:
    """genai.Client must be constructed with an http_options timeout."""
    monkeypatch.setenv("GEMINI_API_KEY", "test-key")
    clients_created: list = []

    import google.genai as genai_module

    def capturing_client(**kwargs):
        clients_created.append(kwargs)
        mock = MagicMock()
        return mock

    monkeypatch.setattr(genai_module, "Client", capturing_client)

    km = GeminiKeyManager()

    def fake_fn(client, key):
        return "ok"

    km.run_with_rotation(fake_fn)
    assert clients_created, "Client was never constructed"
    kwargs = clients_created[0]
    http_options = kwargs.get("http_options")
    assert http_options is not None, "http_options not passed to genai.Client"
    timeout = getattr(http_options, "timeout", None)
    assert timeout == _HTTP_TIMEOUT_MS, f"Expected timeout={_HTTP_TIMEOUT_MS}, got {timeout!r}"
