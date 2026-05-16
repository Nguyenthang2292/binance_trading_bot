from __future__ import annotations

import os
import secrets
from dataclasses import dataclass
from typing import Callable, TypeVar

from google import genai
from google.genai import errors, types as genai_types

T = TypeVar("T")

_HTTP_TIMEOUT_MS = 30_000  # 30 seconds


@dataclass(frozen=True)
class GeminiKey:
    name: str
    value: str


def _is_retryable_key_error(exc: Exception) -> bool:
    if isinstance(exc, errors.APIError):
        code = getattr(exc, "code", None) or getattr(exc, "status_code", None)
        return code in {401, 429, 500, 502, 503, 504}
    lowered = str(exc).lower()
    return any(
        token in lowered
        for token in ("quota", "rate", "429", "resource_exhausted", "api key", "unavailable")
    )


class GeminiKeyManager:
    def __init__(self) -> None:
        keys: list[GeminiKey] = []
        index = 1
        while True:
            value = os.getenv(f"GEMINI_API_KEY_{index}")
            if not value:
                break
            keys.append(GeminiKey(name=f"GEMINI_API_KEY_{index}", value=value))
            index += 1

        single = os.getenv("GEMINI_API_KEY")
        if not keys and single:
            keys.append(GeminiKey(name="GEMINI_API_KEY", value=single))

        packed_keys = os.getenv("GEMINI_API_KEYS")
        if not keys and packed_keys:
            for packed_index, value in enumerate(_split_key_list(packed_keys), start=1):
                keys.append(GeminiKey(name=f"GEMINI_API_KEYS[{packed_index}]", value=value))

        text_key = os.getenv("GEMINI_TEXT_API_KEY")
        if not keys and text_key:
            keys.append(GeminiKey(name="GEMINI_TEXT_API_KEY", value=text_key))

        if not keys:
            raise RuntimeError("No Gemini API key found in environment")

        self._keys = keys
        self._start = secrets.randbelow(len(keys))

    def run_with_rotation(self, fn: Callable[[genai.Client, GeminiKey], T]) -> T:
        last_error: Exception | None = None
        for offset in range(len(self._keys)):
            key = self._keys[(self._start + offset) % len(self._keys)]
            client = genai.Client(
                api_key=key.value,
                http_options=genai_types.HttpOptions(timeout=_HTTP_TIMEOUT_MS),
            )
            try:
                return fn(client, key)
            except Exception as exc:
                if not _is_retryable_key_error(exc):
                    raise
                last_error = exc
        raise RuntimeError(f"All {len(self._keys)} Gemini keys failed: {last_error}")


def _split_key_list(raw: str) -> list[str]:
    normalized = raw.replace(";", ",").replace("\n", ",")
    return [part.strip() for part in normalized.split(",") if part.strip()]
