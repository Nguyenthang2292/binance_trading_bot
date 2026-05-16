from __future__ import annotations

import logging
from pathlib import Path
from typing import Any

import pandas as pd  # type: ignore
import plotly.graph_objects as go  # type: ignore
from plotly.subplots import make_subplots  # type: ignore

LOGGER = logging.getLogger("gemini_filter.chart")


def _sanitize_component(value: str) -> str:
    cleaned = "".join(ch if ch.isalnum() or ch in ("-", "_") else "_" for ch in value)
    return cleaned or "unknown"


def _to_ohlc_dataframe(klines: list[dict[str, Any]]) -> pd.DataFrame:
    frame = pd.DataFrame(klines)
    frame["Date"] = pd.to_datetime(frame["open_time"], unit="ms", utc=True)
    frame = frame.rename(
        columns={
            "open": "Open",
            "high": "High",
            "low": "Low",
            "close": "Close",
            "volume": "Volume",
        }
    )
    frame = frame[["Date", "Open", "High", "Low", "Close", "Volume"]].copy()
    for column in ("Open", "High", "Low", "Close", "Volume"):
        frame[column] = frame[column].astype(float)
    return frame.sort_values("Date")


def _row_heights(timeframe_count: int) -> list[float]:
    if timeframe_count <= 1:
        return [1.0]
    primary_height = 0.48
    extra_height = (1.0 - primary_height) / float(timeframe_count - 1)
    return [extra_height] * (timeframe_count - 1) + [primary_height]


def generate_chart(
    *,
    klines_by_tf: dict[str, list[dict[str, Any]]],
    primary_tf: str,
    symbol: str,
    output_dir: str,
    eval_id: str,
) -> str:
    output_root = Path(output_dir)
    output_root.mkdir(parents=True, exist_ok=True)
    output_path = output_root / (
        f"{_sanitize_component(symbol)}_{_sanitize_component(primary_tf)}_{_sanitize_component(eval_id)}.png"
    )

    extra_tfs = [tf for tf in klines_by_tf.keys() if tf != primary_tf]
    ordered_tfs = extra_tfs + [primary_tf]
    if not ordered_tfs:
        raise RuntimeError("No timeframe data provided for chart generation")

    LOGGER.info(
        "chart render start eval_id=%s symbol=%s primary_tf=%s tfs=%s",
        eval_id,
        symbol,
        primary_tf,
        ",".join(ordered_tfs),
    )
    fig: Any = make_subplots(
        rows=len(ordered_tfs),
        cols=1,
        shared_xaxes=False,
        vertical_spacing=0.055,
        row_heights=_row_heights(len(ordered_tfs)),
        specs=[[{"secondary_y": True}] for _ in ordered_tfs],
        subplot_titles=[
            f"{symbol} {tf}" + ("  PRIMARY" if tf == primary_tf else "")
            for tf in ordered_tfs
        ],
    )

    for row_index, tf in enumerate(ordered_tfs, start=1):
        frame = _to_ohlc_dataframe(klines_by_tf[tf]).tail(100)
        increasing = frame["Close"] >= frame["Open"]
        volume_colors = [
            "rgba(15, 157, 88, 0.22)" if is_up else "rgba(219, 68, 55, 0.22)"
            for is_up in increasing
        ]

        fig.add_trace(
            go.Candlestick(
                x=frame["Date"],
                open=frame["Open"],
                high=frame["High"],
                low=frame["Low"],
                close=frame["Close"],
                increasing_line_color="#0f9d58",
                increasing_fillcolor="rgba(15, 157, 88, 0.72)",
                decreasing_line_color="#db4437",
                decreasing_fillcolor="rgba(219, 68, 55, 0.72)",
                name=f"{tf} candles",
                showlegend=False,
            ),
            row=row_index,
            col=1,
            secondary_y=False,
        )
        fig.add_trace(
            go.Bar(
                x=frame["Date"],
                y=frame["Volume"],
                marker_color=volume_colors,
                name=f"{tf} volume",
                opacity=0.35,
                showlegend=False,
            ),
            row=row_index,
            col=1,
            secondary_y=True,
        )
        fig.update_xaxes(rangeslider_visible=False, showgrid=True, gridcolor="#263240", row=row_index, col=1)
        fig.update_yaxes(showgrid=True, gridcolor="#263240", row=row_index, col=1, secondary_y=False)
        fig.update_yaxes(showgrid=False, visible=False, row=row_index, col=1, secondary_y=True)

    fig.update_layout(
        template="plotly_dark",
        paper_bgcolor="#0d1117",
        plot_bgcolor="#111827",
        margin=dict(l=48, r=24, t=48, b=36),
        height=max(480, 320 * len(ordered_tfs)),
        width=1280,
        font=dict(family="DejaVu Sans, Segoe UI, sans-serif", size=13, color="#e5e7eb"),
    )
    fig.write_image(
        str(output_path),
        format="png",
        width=1280,
        height=max(480, 320 * len(ordered_tfs)),
        scale=1,
    )
    LOGGER.info("chart render completed eval_id=%s path=%s", eval_id, output_path)
    return str(output_path)
