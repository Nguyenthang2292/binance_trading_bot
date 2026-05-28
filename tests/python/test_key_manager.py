from __future__ import annotations

import os
import time
from contextlib import contextmanager

import pytest
from unittest.mock import MagicMock
from google.genai import errors as genai_errors

import tools.shared.gemini_key_manager as gkm
from tools.shared.gemini_key_manager import (
    _HTTP_TIMEOUT_MS,
    GeminiKey,
    GeminiKeyManager,
    _file_lock,
    _keyset_digest,
    _is_retryable_key_error,
    _split_key_list,
)


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


def test_500_is_not_retryable() -> None:
    assert not _is_retryable_key_error(_api_error(500))


def test_502_is_not_retryable() -> None:
    assert not _is_retryable_key_error(_api_error(502))


def test_503_is_not_retryable() -> None:
    assert not _is_retryable_key_error(_api_error(503))


def test_504_is_not_retryable() -> None:
    assert not _is_retryable_key_error(_api_error(504))


def test_timeout_text_is_not_retryable() -> None:
    assert not _is_retryable_key_error(RuntimeError("504 deadline exceeded while waiting for response"))


def test_generic_rate_word_is_not_retryable() -> None:
    assert not _is_retryable_key_error(RuntimeError("model hit a sampling rate mismatch"))


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
        "GEMINI_API_KEYS_1",
        "GEMINI_API_KEYS_2",
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
        "GEMINI_API_KEYS_2",
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
            raise RuntimeError("429 resource_exhausted")
        return "ok"

    assert GeminiKeyManager(state_dir=tmp_path).run_with_rotation(flaky) == "ok"

    assert api_keys == ["key-1", "key-2"]


def test_default_state_dir_is_absolute_and_not_cwd_relative(monkeypatch: pytest.MonkeyPatch) -> None:
    _clear_gemini_env(monkeypatch)
    monkeypatch.setenv("GEMINI_API_KEY", "single-key")

    km = GeminiKeyManager()

    assert km._state_dir.is_absolute()
    assert km._state_dir.name == "gemini_key_manager"
    assert "tmp" in str(km._state_dir).lower() or "temp" in str(km._state_dir).lower()


def test_file_lock_reclaims_stale_marker(tmp_path) -> None:
    lock_path = tmp_path / "state.lock"
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock_path.write_text("stale", encoding="utf-8")
    old = time.time() - 120
    os.utime(lock_path, (old, old))

    with _file_lock(lock_path, timeout_seconds=0.2, stale_seconds=0.01):
        assert lock_path.exists()


def test_client_closed_after_successful_call(tmp_path, monkeypatch: pytest.MonkeyPatch) -> None:
    _clear_gemini_env(monkeypatch)
    monkeypatch.setenv("GEMINI_API_KEY", "key-1")

    clients: list[MagicMock] = []
    import google.genai as genai_module

    def make_client(**_kwargs):
        client = MagicMock()
        clients.append(client)
        return client

    monkeypatch.setattr(genai_module, "Client", make_client)

    km = GeminiKeyManager(state_dir=tmp_path)
    assert km.run_with_rotation(lambda _client, _key: "ok") == "ok"

    assert len(clients) == 1
    clients[0].close.assert_called_once()


def test_client_closed_for_each_retry_attempt(tmp_path, monkeypatch: pytest.MonkeyPatch) -> None:
    _clear_gemini_env(monkeypatch)
    monkeypatch.setenv("GEMINI_API_KEYS", "key-1,key-2")

    clients: list[MagicMock] = []
    import google.genai as genai_module

    def make_client(**_kwargs):
        client = MagicMock()
        clients.append(client)
        return client

    monkeypatch.setattr(genai_module, "Client", make_client)

    calls = 0

    def flaky(_client, _key):
        nonlocal calls
        calls += 1
        if calls == 1:
            raise RuntimeError("429 resource_exhausted")
        return "ok"

    km = GeminiKeyManager(state_dir=tmp_path)
    assert km.run_with_rotation(flaky) == "ok"

    assert len(clients) == 2
    clients[0].close.assert_called_once()
    clients[1].close.assert_called_once()


def test_server_error_does_not_rotate_all_keys(tmp_path, monkeypatch: pytest.MonkeyPatch) -> None:
    _clear_gemini_env(monkeypatch)
    monkeypatch.setenv("GEMINI_API_KEYS", "key-1,key-2,key-3")

    clients: list[MagicMock] = []
    import google.genai as genai_module

    def make_client(**_kwargs):
        client = MagicMock()
        clients.append(client)
        return client

    monkeypatch.setattr(genai_module, "Client", make_client)

    km = GeminiKeyManager(state_dir=tmp_path)

    def server_error(_client, _key):
        raise RuntimeError("upstream 503")

    with pytest.raises(RuntimeError, match="upstream 503"):
        km.run_with_rotation(server_error)

    assert len(clients) == 1
    clients[0].close.assert_called_once()


def test_all_retryable_failures_raise_from_last_error(tmp_path, monkeypatch: pytest.MonkeyPatch) -> None:
    _clear_gemini_env(monkeypatch)
    monkeypatch.setenv("GEMINI_API_KEYS", "key-1,key-2")
    km = GeminiKeyManager(state_dir=tmp_path)

    def quota_error(_client, _key):
        raise RuntimeError("429 resource_exhausted")

    with pytest.raises(RuntimeError, match="All 2 Gemini keys failed") as exc_info:
        km.run_with_rotation(quota_error)
    assert exc_info.value.__cause__ is not None
    assert "429 resource_exhausted" in str(exc_info.value.__cause__)


def test_fallback_start_index_uses_random_when_state_claim_fails(
    tmp_path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _clear_gemini_env(monkeypatch)
    monkeypatch.setenv("GEMINI_API_KEYS", "key-1,key-2,key-3")

    @contextmanager
    def failing_lock(_path, timeout_seconds=2.0, stale_seconds=30.0):
        raise TimeoutError("lock failed")
        yield  # pragma: no cover

    monkeypatch.setattr(gkm, "_file_lock", failing_lock)
    monkeypatch.setattr(gkm.random, "randrange", lambda n: 2)

    km = GeminiKeyManager(state_dir=tmp_path)
    assert km._claim_start_index() == 2


def test_keyset_digest_depends_on_values_not_names() -> None:
    first = [GeminiKey(name="GEMINI_API_KEY_1", value="k1"), GeminiKey(name="GEMINI_API_KEY", value="k2")]
    second = [GeminiKey(name="GEMINI_API_KEYS_1", value="k1"), GeminiKey(name="GEMINI_TEXT_API_KEY", value="k2")]
    assert _keyset_digest(first) == _keyset_digest(second)
