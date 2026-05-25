from __future__ import annotations

import os

import pytest
from unittest.mock import MagicMock
from google.genai import errors as genai_errors

from tools.shared.gemini_key_manager import GeminiKeyManager, _is_retryable_key_error, _split_key_list, _HTTP_TIMEOUT_MS


def test_split_key_list_supports_multiple_delimiters() -> None:
    parsed = _split_key_list("k1;k2,k3\nk4")
    assert parsed == ["k1", "k2", "k3", "k4"]


def _api_error(code: int) -> Exception:
    exc = MagicMock(spec=genai_errors.APIError)
    exc.code = code
    exc.status_code = None
    return exc


def _clear_gemini_env(monkeypatch: pytest.MonkeyPatch) -> None:
    for name in list(os.environ):
        if name.startswith("GEMINI_API_KEY") or name in {"GEMINI_TEXT_API_KEY", "GEMINI_KEY_MANAGER_STATE_DIR"}:
            monkeypatch.delenv(name, raising=False)


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


def test_client_constructed_with_overridden_timeout(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("GEMINI_API_KEY", "test-key")
    clients_created: list = []

    import google.genai as genai_module

    def capturing_client(**kwargs):
        clients_created.append(kwargs)
        return MagicMock()

    monkeypatch.setattr(genai_module, "Client", capturing_client)

    km = GeminiKeyManager(http_timeout_ms=6_000)
    km.run_with_rotation(lambda _client, _key: "ok")

    assert clients_created, "Client was never constructed"
    kwargs = clients_created[0]
    http_options = kwargs.get("http_options")
    timeout = getattr(http_options, "timeout", None)
    assert timeout == 6_000, f"Expected timeout=6000, got {timeout!r}"


def test_collects_all_key_sources_when_single_key_is_present(
    tmp_path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _clear_gemini_env(monkeypatch)
    monkeypatch.setenv("GEMINI_API_KEY", "single-key")
    monkeypatch.setenv("GEMINI_API_KEYS", "packed-1,packed-2")
    monkeypatch.setenv("GEMINI_TEXT_API_KEY", "text-key")

    km = GeminiKeyManager(state_dir=tmp_path)

    assert [key.name for key in km._keys] == [
        "GEMINI_API_KEY",
        "GEMINI_API_KEYS[1]",
        "GEMINI_API_KEYS[2]",
        "GEMINI_TEXT_API_KEY",
    ]
    assert [key.value for key in km._keys] == ["single-key", "packed-1", "packed-2", "text-key"]


def test_deduplicates_key_values_across_sources(tmp_path, monkeypatch: pytest.MonkeyPatch) -> None:
    _clear_gemini_env(monkeypatch)
    monkeypatch.setenv("GEMINI_API_KEY_1", "shared-key")
    monkeypatch.setenv("GEMINI_API_KEY", "single-key")
    monkeypatch.setenv("GEMINI_API_KEYS", "shared-key,packed-key")

    km = GeminiKeyManager(state_dir=tmp_path)

    assert [key.name for key in km._keys] == [
        "GEMINI_API_KEY_1",
        "GEMINI_API_KEY",
        "GEMINI_API_KEYS[2]",
    ]
    assert [key.value for key in km._keys] == ["shared-key", "single-key", "packed-key"]


def test_round_robin_start_persists_across_manager_instances(
    tmp_path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _clear_gemini_env(monkeypatch)
    monkeypatch.setenv("GEMINI_API_KEYS", "key-1,key-2,key-3")
    api_keys: list[str] = []

    import google.genai as genai_module

    def capturing_client(**kwargs):
        api_keys.append(kwargs["api_key"])
        return MagicMock()

    monkeypatch.setattr(genai_module, "Client", capturing_client)

    for _ in range(4):
        GeminiKeyManager(state_dir=tmp_path).run_with_rotation(lambda _client, _key: "ok")

    assert api_keys == ["key-1", "key-2", "key-3", "key-1"]


def test_retryable_error_moves_to_next_key_from_round_robin_start(
    tmp_path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _clear_gemini_env(monkeypatch)
    monkeypatch.setenv("GEMINI_API_KEYS", "key-1,key-2")
    api_keys: list[str] = []

    import google.genai as genai_module

    def capturing_client(**kwargs):
        api_keys.append(kwargs["api_key"])
        return MagicMock()

    monkeypatch.setattr(genai_module, "Client", capturing_client)

    calls = 0

    def flaky(_client, _key):
        nonlocal calls
        calls += 1
        if calls == 1:
            raise RuntimeError("temporary unavailable")
        return "ok"

    assert GeminiKeyManager(state_dir=tmp_path).run_with_rotation(flaky) == "ok"

    assert api_keys == ["key-1", "key-2"]
