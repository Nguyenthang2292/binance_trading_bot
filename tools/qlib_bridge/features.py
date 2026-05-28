"""Build the feature set used by the qlib bridge training and inference jobs.

The feature engineering pipeline works on symbol-partitioned OHLCV data and
adds the rolling returns, moving-average distances, breakout levels, volume
anomalies, and ATR-style volatility signals consumed by the downstream model
training and scoring scripts.

The exported ``FEATURE_COLS`` list defines the canonical feature order so the
rest of the qlib bridge can train, validate, and score against a stable schema.
"""

from __future__ import annotations

import numpy as np
import pandas as pd  # type: ignore[import]

FEATURE_COLS: list[str] = [
    "ret_1",
    "ret_4",
    "ret_12",
    "ma_dist_8",
    "ma_dist_24",
    "vol_24",
    "breakout_high_24",
    "breakout_low_24",
    "volume_z_24",
    "atr_14",
]


def add_features(frame: pd.DataFrame) -> pd.DataFrame:
    frame = frame.copy()
    grp = frame.groupby("symbol", group_keys=False)

    frame["ret_1"] = grp["close"].pct_change(1)
    frame["ret_4"] = grp["close"].pct_change(4)
    frame["ret_12"] = grp["close"].pct_change(12)

    ma_8 = grp["close"].transform(lambda s: s.rolling(8, min_periods=8).mean())
    ma_24 = grp["close"].transform(lambda s: s.rolling(24, min_periods=24).mean())
    frame["ma_dist_8"] = (frame["close"] / ma_8.replace(0.0, np.nan)) - 1.0
    frame["ma_dist_24"] = (frame["close"] / ma_24.replace(0.0, np.nan)) - 1.0

    frame["vol_24"] = grp["ret_1"].transform(lambda s: s.rolling(24, min_periods=24).std())
    high_24 = grp["high"].transform(lambda s: s.rolling(24, min_periods=24).max())
    low_24 = grp["low"].transform(lambda s: s.rolling(24, min_periods=24).min())
    frame["breakout_high_24"] = (frame["close"] / high_24) - 1.0
    frame["breakout_low_24"] = (frame["close"] / low_24) - 1.0

    vol_mean_24 = grp["volume"].transform(lambda s: s.rolling(24, min_periods=24).mean())
    vol_std_24 = grp["volume"].transform(lambda s: s.rolling(24, min_periods=24).std())
    frame["volume_z_24"] = (frame["volume"] - vol_mean_24) / vol_std_24.replace(0.0, np.nan)

    tr0 = frame["high"] - frame["low"]
    prev_close = grp["close"].shift(1)
    tr1 = (frame["high"] - prev_close).abs()
    tr2 = (frame["low"] - prev_close).abs()
    true_range = pd.concat([tr0, tr1, tr2], axis=1).max(axis=1)
    frame["atr_14"] = true_range.groupby(frame["symbol"]).transform(
        lambda s: s.rolling(14, min_periods=14).mean()
    )
    return frame
