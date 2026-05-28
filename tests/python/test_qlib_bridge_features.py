from __future__ import annotations

import os
import sys
from decimal import Decimal

import numpy as np
import pandas as pd
import pytest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "../../tools/qlib_bridge")))
import features
import run_execution_plan_watcher as watcher


def test_add_features_has_expected_columns_and_no_inf_ma_distance() -> None:
    base_time = pd.Timestamp("2026-01-01 00:00:00", tz="UTC")
    rows = []
    for idx in range(40):
        close = 0.0 if idx < 30 else float(idx)
        rows.append(
            {
                "datetime": base_time + pd.Timedelta(hours=idx),
                "symbol": "BTCUSDT",
                "open": close,
                "high": close,
                "low": close,
                "close": close,
                "volume": float(idx + 1),
            }
        )
    frame = pd.DataFrame(rows)
    enriched = features.add_features(frame)

    assert set(features.FEATURE_COLS).issubset(enriched.columns)
    assert not np.isinf(enriched["ma_dist_8"].to_numpy(dtype=float)).any()
    assert not np.isinf(enriched["ma_dist_24"].to_numpy(dtype=float)).any()


def test_split_quantity_preserves_total_and_quantization() -> None:
    slices = watcher.split_quantity("1", 3)
    assert len(slices) == 3
    assert sum(Decimal(value) for value in slices) == Decimal("1")


def test_split_quantity_rejects_non_positive() -> None:
    with pytest.raises(ValueError, match="quantity must be positive"):
        watcher.split_quantity("0", 3)
