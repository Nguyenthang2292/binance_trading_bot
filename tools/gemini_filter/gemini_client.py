from __future__ import annotations

import json
import math
from typing import Any, cast

from google.genai import types

SCORE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "properties": {
        "score": {
            "type": "number",
            "minimum": 0.0,
            "maximum": 1.0,
            "description": "Favorability score for the proposed trade direction.",
        },
        "analysis": {
            "type": "string",
            "description": "Short explanation of the score.",
        },
    },
    "required": ["score", "analysis"],
}


def _extract_text(response: Any) -> str:
    text = getattr(response, "text", None)
    if isinstance(text, str) and text.strip():
        return text
    raise RuntimeError("Gemini response does not contain text")


def parse_score_payload(payload: dict[str, Any]) -> dict[str, Any]:
    if "score" not in payload or "analysis" not in payload:
        raise RuntimeError("Missing score or analysis fields")

    score = float(payload["score"])
    if not math.isfinite(score) or score < 0.0 or score > 1.0:
        raise RuntimeError("Score must be finite and in [0,1]")

    analysis = payload["analysis"]
    if not isinstance(analysis, str):
        raise RuntimeError("Analysis must be a string")

    return {"score": score, "analysis": analysis.strip()}


def parse_score_text(text: str) -> dict[str, Any]:
    parsed = json.loads(text)
    if not isinstance(parsed, dict):
        raise RuntimeError("Score payload must be a JSON object")
    parsed_typed = cast(dict[str, Any], parsed)
    return parse_score_payload(parsed_typed)


def generate_json_score(
    *,
    client: Any,
    model: str,
    contents: Any,
    use_google_search: bool,
) -> dict[str, Any]:
    if use_google_search:
        # Google Search grounding is incompatible with JSON schema output mode.
        # Request plain text with grounding; the model is still prompted to return JSON.
        config = types.GenerateContentConfig(
            tools=[types.Tool(google_search=types.GoogleSearch())]
        )
    else:
        config = types.GenerateContentConfig(
            response_mime_type="application/json",
            response_schema=SCORE_SCHEMA,
        )
    response = client.models.generate_content(
        model=model,
        contents=contents,
        config=config,
    )
    return parse_score_text(_extract_text(response))


def generate_plain_text(
    *,
    client: Any,
    model: str,
    contents: Any,
    use_google_search: bool,
) -> str:
    config = None
    if use_google_search:
        config = types.GenerateContentConfig(
            tools=[types.Tool(google_search=types.GoogleSearch())]
        )
    response = client.models.generate_content(
        model=model,
        contents=contents,
        config=config,
    )
    return _extract_text(response)
