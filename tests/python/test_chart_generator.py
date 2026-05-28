from __future__ import annotations

import pytest

from tools.gemini_filter.chart_generator import (
    _row_heights,
    _sanitize_component,
    _to_ohlc_dataframe,
)


def test_sanitize_component_replaces_invalid_chars_and_handles_empty() -> None:
    assert _sanitize_component("BTC/USDT:1h*?") == "BTC_USDT_1h__"
    assert _sanitize_component("") == "unknown"


def test_row_heights_balances_primary_panel() -> None:
    assert _row_heights(1) == [1.0]
    heights = _row_heights(3)
    assert len(heights) == 3
    assert heights[-1] == pytest.approx(0.48)
    assert heights[0] == pytest.approx(0.26)
    assert heights[1] == pytest.approx(0.26)
    assert sum(heights) == pytest.approx(1.0)


def test_to_ohlc_dataframe_sorts_by_time_and_casts_numeric_columns() -> None:
    frame = _to_ohlc_dataframe(
        [
            {
                "open_time": 2_000,
                "open": "2.0",
                "high": "2.5",
                "low": "1.5",
                "close": "2.2",
                "volume": "11",
            },
            {
                "open_time": 1_000,
                "open": "1.0",
                "high": "1.5",
                "low": "0.5",
                "close": "1.2",
                "volume": "10",
            },
        ]
    )

    assert list(frame.columns) == ["Date", "Open", "High", "Low", "Close", "Volume"]
    assert frame["Date"].iloc[0] < frame["Date"].iloc[1]
    for column in ("Open", "High", "Low", "Close", "Volume"):
        assert frame[column].dtype.kind == "f"
    assert frame["Open"].iloc[0] == pytest.approx(1.0)
    assert frame["Close"].iloc[1] == pytest.approx(2.2)


def test_to_ohlc_dataframe_rejects_missing_required_fields() -> None:
    with pytest.raises(RuntimeError, match="missing fields"):
        _to_ohlc_dataframe([{"open_time": 1_000, "open": "1.0"}])


def test_to_ohlc_dataframe_rejects_bad_open_time() -> None:
    with pytest.raises(RuntimeError, match="open_time"):
        _to_ohlc_dataframe(
            [
                {
                    "open_time": "not-ms",
                    "open": "1.0",
                    "high": "1.5",
                    "low": "0.5",
                    "close": "1.2",
                    "volume": "10",
                }
            ]
        )
