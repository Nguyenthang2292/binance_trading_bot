from __future__ import annotations

from tools.gemini_filter.key_manager import _split_key_list


def test_split_key_list_supports_multiple_delimiters() -> None:
    parsed = _split_key_list("k1;k2,k3\nk4")
    assert parsed == ["k1", "k2", "k3", "k4"]

