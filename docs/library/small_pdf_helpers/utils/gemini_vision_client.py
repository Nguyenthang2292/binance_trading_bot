from __future__ import annotations

import io
import re
from types import SimpleNamespace
from typing import Any, Callable, TypeVar, cast

from PIL import Image

try:
    from .gemini_key_manager import GeminiKeyManager
except ImportError:
    from gemini_key_manager import GeminiKeyManager

try:
    from google.genai import types  # type: ignore[import-not-found]
except Exception:  # pragma: no cover - optional dependency during static checks
    class _FallbackPart:
        def __init__(self, *, data: bytes, mime_type: str) -> None:
            self.inline_data = SimpleNamespace(data=data, mime_type=mime_type)

        @classmethod
        def from_bytes(cls, *, data: bytes, mime_type: str) -> "_FallbackPart":
            return cls(data=data, mime_type=mime_type)

    types = cast(Any, SimpleNamespace(Part=_FallbackPart))


_AUTO_MODEL_VALUES = {"", "auto", "latest"}
_FALLBACK_MODEL = "gemini-3.1-flash-image-preview"
T = TypeVar("T")

_HEXAGRAM_PROMPT = """Ảnh này là một trang sách tiếng Việt về Lục Hào (kinh dịch).
Nếu trang có bảng quẻ, hãy trích xuất và trả về Markdown theo format sau:

1. Tên quẻ chính và biến quẻ dạng heading
2. Hexagram symbols: mỗi vạch dương = ━━━━━━, vạch âm = ━━  ━━ (6 vạch, từ trên xuống)
3. Bảng dữ liệu đầy đủ với các cột:
   Hào | T/Ư | Lục Thân | Chi | Phục | TK | Lục Thân | Chi | TK | Lục Thủ | Hào
4. Dòng TUẦN KHÔNG nếu có

Nếu trang KHÔNG có bảng quẻ, trả về chuỗi: NO_HEXAGRAM_TABLE
Chỉ trả về Markdown, không giải thích thêm."""

_FULL_PAGE_PROMPT = """Ảnh này là một trang sách tiếng Việt về Lục Hào (kinh dịch).
Hãy trích xuất TOÀN BỘ nội dung trang này thành Markdown:

- Tiêu đề, heading → dùng ## hoặc ###
- Đoạn văn thường → giữ nguyên
- Bảng quẻ hexagram → trích xuất đầy đủ theo format bảng Markdown
  (Hào | T/Ư | Lục Thân | Chi | Phục | TK | ...)
- Hexagram symbols: vạch dương = ━━━━━━, vạch âm = ━━  ━━
- Số thứ tự, ghi chú → giữ nguyên

Chỉ trả về Markdown thuần, không giải thích, không thêm nội dung ngoài trang."""


class GeminiVisionClient:
    """Gemini vision client for extracting Markdown from PDF page images."""

    def __init__(
        self,
        api_key: str | None = None,
        model: str = "gemini-3.1-pro-preview",
        client: Any | None = None,
        key_manager: GeminiKeyManager | None = None,
    ):
        self.api_key = api_key or ""
        self._requested_model = model.strip()
        self.model = self._requested_model or "gemini-3.1-pro-preview"
        self._key_manager = key_manager
        self._client = client or (self._build_client(api_key) if api_key else None)
        self._resolved_model: str | None = None

        if self._client is None and self._key_manager is None:
            raise ValueError("GeminiVisionClient requires api_key, client, or key_manager")

    @classmethod
    def from_environment(
        cls,
        model: str = "gemini-3.1-pro-preview",
    ) -> "GeminiVisionClient":
        return cls(
            api_key=None,
            model=model,
            key_manager=GeminiKeyManager(),
        )

    @staticmethod
    def _build_client(api_key: str) -> Any:
        try:
            from google import genai  # type: ignore[import-not-found]
        except ImportError as exc:
            raise ImportError(
                "google-genai package is required for GeminiVisionClient",
            ) from exc

        return genai.Client(api_key=api_key)

    def extract_hexagram_table(self, image: Image.Image) -> str:
        model = self._resolve_model_name()
        response = self._generate_content(
            model=model,
            contents=[_HEXAGRAM_PROMPT, self._image_part(image)],
        )
        return self._parse_response(response)

    def extract_full_page(self, image: Image.Image) -> str:
        """Trích xuất toàn bộ nội dung một trang PDF thành Markdown.

        Dùng cho Gemini conversion flow.
        Khác với extract_hexagram_table() chỉ dùng cho repair flow.
        """
        model = self._resolve_model_name()
        response = self._generate_content(
            model=model,
            contents=[_FULL_PAGE_PROMPT, self._image_part(image)],
        )
        return self._parse_response(response)

    def _run_with_client(self, fn: Callable[[Any], T]) -> T:
        if self._key_manager is not None:
            return self._key_manager.run_with_rotation(lambda client, _key: fn(client))

        if self._client is None:
            raise RuntimeError("Gemini client not initialized")
        return fn(self._client)

    def _generate_content(self, *, model: str, contents: list[Any]) -> Any:
        return self._run_with_client(
            lambda client: client.models.generate_content(
                model=model,
                contents=contents,
            )
        )

    def _resolve_model_name(self) -> str:
        requested_model = self._requested_model.lower()
        if requested_model not in _AUTO_MODEL_VALUES:
            self._resolved_model = self._requested_model
            self.model = self._resolved_model
            return self._resolved_model

        if self._resolved_model is None:
            self._resolved_model = self._discover_latest_model()
            self.model = self._resolved_model
        return self._resolved_model

    def _discover_latest_model(self) -> str:
        candidates: list[str] = []
        model_list = self._run_with_client(lambda client: list(client.models.list()))
        for item in model_list:
            name = getattr(item, "name", None)
            if not isinstance(name, str):
                continue

            model_name = name.rsplit("/", maxsplit=1)[-1]
            if not re.match(r"^gemini-\d", model_name):
                continue
            if not self._supports_generate_content(item):
                continue

            candidates.append(model_name)

        if not candidates:
            return _FALLBACK_MODEL

        return max(candidates, key=self._model_rank)

    @staticmethod
    def _supports_generate_content(model: Any) -> bool:
        methods = getattr(model, "supported_generation_methods", None)
        if methods is None:
            methods = getattr(model, "supportedGenerationMethods", None)

        if not isinstance(methods, list):
            return True

        normalized = {str(method).strip().lower() for method in cast(list[Any], methods)}
        return "generatecontent" in normalized

    @staticmethod
    def _model_rank(model_name: str) -> tuple[int, ...]:
        numbers = tuple(int(token) for token in re.findall(r"\d+", model_name))
        if not numbers:
            return (0,)

        model_lower = model_name.lower()
        image_bonus = 2 if "image" in model_lower else 0
        preview_bonus = 1 if any(token in model_lower for token in ("preview", "exp", "experimental")) else 0
        return numbers + (image_bonus, preview_bonus)

    @staticmethod
    def _image_part(image: Image.Image) -> types.Part:
        buffer = io.BytesIO()
        image.save(buffer, format="PNG")
        return types.Part.from_bytes(data=buffer.getvalue(), mime_type="image/png")

    def _parse_response(self, response: Any) -> str:
        text = getattr(response, "text", None)
        if isinstance(text, str) and text.strip():
            return text.strip()

        parts = self._collect_text_parts(getattr(response, "candidates", None))
        if parts:
            return "\n".join(parts)

        raise ValueError("Gemini response does not contain text content")

    def _collect_text_parts(self, value: Any) -> list[str]:
        parts: list[str] = []

        if isinstance(value, str):
            stripped = value.strip()
            if stripped:
                parts.append(stripped)
            return parts

        if isinstance(value, list):
            for item in cast(list[Any], value):
                parts.extend(self._collect_text_parts(item))
            return parts

        if value is None:
            return parts

        text = self._explicit_attr(value, "text")
        if isinstance(text, str) and text.strip():
            parts.append(text.strip())

        nested_parts = self._explicit_attr(value, "parts")
        if nested_parts is not None:
            parts.extend(self._collect_text_parts(nested_parts))

        content = self._explicit_attr(value, "content")
        if content is not None:
            parts.extend(self._collect_text_parts(content))

        return parts

    @staticmethod
    def _explicit_attr(value: Any, name: str) -> Any:
        if isinstance(value, dict):
            return value.get(name)

        data = getattr(value, "__dict__", None)
        if isinstance(data, dict) and name in data:
            return data[name]

        return None


__all__ = ["GeminiVisionClient"]
