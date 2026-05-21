from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

# Some third-party packages used at runtime do not provide type stubs
# in this workspace. Silence Pyright/mypy import errors here.
import lightgbm as lgb  # type: ignore[import]
import numpy as np
import pandas as pd  # type: ignore[import]

from features import FEATURE_COLS, add_features


INTERVAL_TO_DELTA = {
    "30m": pd.Timedelta(minutes=30),
    "1h": pd.Timedelta(hours=1),
    "4h": pd.Timedelta(hours=4),
    "1d": pd.Timedelta(days=1),
}


@dataclass(frozen=True)
class FoldResult:
    train_start: str
    train_end: str
    test_start: str
    test_end: str
    train_rows: int
    test_rows: int
    ic: float
    rank_ic: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Train LightGBM baseline with walk-forward validation.")
    parser.add_argument("--dataset", required=True, help="Input csv/parquet exported by qlib bridge")
    parser.add_argument("--interval", required=True, choices=["30m", "1h", "4h", "1d"])
    parser.add_argument("--horizon-bars", type=int, required=True)
    parser.add_argument("--model-out", required=True)
    parser.add_argument("--report-out", required=True)
    parser.add_argument("--train-window-days", type=int, default=180)
    parser.add_argument("--test-window-days", type=int, default=30)
    parser.add_argument("--num-boost-round", type=int, default=300)
    parser.add_argument("--learning-rate", type=float, default=0.05)
    parser.add_argument("--num-leaves", type=int, default=31)
    parser.add_argument("--feature-fraction", type=float, default=0.8)
    parser.add_argument("--bagging-fraction", type=float, default=0.8)
    parser.add_argument("--bagging-freq", type=int, default=5)
    parser.add_argument("--seed", type=int, default=42)
    return parser.parse_args()


def load_dataset(path: Path) -> pd.DataFrame:
    suffix = path.suffix.lower()
    if suffix == ".csv":
        frame = pd.read_csv(path)
    elif suffix == ".parquet":
        frame = pd.read_parquet(path)
    else:
        raise ValueError(f"Unsupported dataset suffix: {suffix}")

    required = {"datetime", "symbol", "open", "high", "low", "close", "volume"}
    missing = sorted(required - set(frame.columns))
    if missing:
        raise ValueError(f"Missing required columns: {missing}")

    frame["datetime"] = pd.to_datetime(frame["datetime"], utc=True)
    frame = frame.sort_values(["symbol", "datetime"], ascending=[True, True]).reset_index(drop=True)
    return frame


def add_label(frame: pd.DataFrame, horizon_bars: int, interval: str) -> pd.DataFrame:
    frame = frame.copy()
    step = INTERVAL_TO_DELTA[interval]
    grp = frame.groupby("symbol", group_keys=False)
    frame["future_close"] = grp["close"].shift(-horizon_bars)
    frame["label"] = (frame["future_close"] / frame["close"]) - 1.0
    frame["label_end_time"] = frame["datetime"] + (step * horizon_bars)
    return frame


def _spearman_corr(x: np.ndarray, y: np.ndarray) -> float:
    if x.size < 2 or y.size < 2:
        return float("nan")
    xr = pd.Series(x).rank(method="average").to_numpy()
    yr = pd.Series(y).rank(method="average").to_numpy()
    xstd = np.std(xr)
    ystd = np.std(yr)
    if xstd == 0.0 or ystd == 0.0:
        return float("nan")
    return float(np.corrcoef(xr, yr)[0, 1])


def _pearson_corr(x: np.ndarray, y: np.ndarray) -> float:
    if x.size < 2 or y.size < 2:
        return float("nan")
    xstd = np.std(x)
    ystd = np.std(y)
    if xstd == 0.0 or ystd == 0.0:
        return float("nan")
    return float(np.corrcoef(x, y)[0, 1])


def walk_forward_splits(
    frame: pd.DataFrame,
    train_window_days: int,
    test_window_days: int,
    horizon_bars: int,
    interval: str,
) -> list[tuple[pd.Timestamp, pd.Timestamp, pd.Timestamp, pd.Timestamp]]:
    step = INTERVAL_TO_DELTA[interval]
    embargo = step * horizon_bars
    test_step = pd.Timedelta(days=test_window_days)
    train_window = pd.Timedelta(days=train_window_days)

    min_dt = frame["datetime"].min()
    max_dt = frame["datetime"].max()

    splits: list[tuple[pd.Timestamp, pd.Timestamp, pd.Timestamp, pd.Timestamp]] = []
    test_start = (min_dt + train_window).floor("D")
    while test_start < max_dt:
        test_end = test_start + test_step
        train_end = test_start - embargo
        train_start = train_end - train_window

        if train_start >= train_end:
            test_start = test_end
            continue
        if test_start >= test_end:
            test_start = test_end
            continue

        splits.append((train_start, train_end, test_start, test_end))
        test_start = test_end
    return splits


def build_fold_data(
    frame: pd.DataFrame,
    feature_cols: list[str],
    train_start: pd.Timestamp,
    train_end: pd.Timestamp,
    test_start: pd.Timestamp,
    test_end: pd.Timestamp,
) -> tuple[pd.DataFrame, pd.DataFrame]:
    base_train = frame[(frame["datetime"] >= train_start) & (frame["datetime"] < train_end)].copy()
    test = frame[(frame["datetime"] >= test_start) & (frame["datetime"] < test_end)].copy()

    # Purging: remove all train rows whose label window overlaps with test window.
    overlap = (base_train["label_end_time"] >= test_start) & (base_train["datetime"] < test_end)
    train = base_train.loc[~overlap].copy()

    train = train.dropna(subset=feature_cols + ["label"])
    test = test.dropna(subset=feature_cols + ["label"])
    return train, test


def compute_percentiles(scores: pd.Series) -> pd.Series:
    if scores.empty:
        return scores
    return scores.rank(method="average", pct=True, ascending=True).clip(0.0, 1.0)


def run_training(args: argparse.Namespace) -> dict[str, Any]:
    dataset_path = Path(args.dataset)
    frame = load_dataset(dataset_path)
    frame = add_features(frame)
    frame = add_label(frame, args.horizon_bars, args.interval)

    feature_cols = FEATURE_COLS

    splits = walk_forward_splits(
        frame=frame,
        train_window_days=args.train_window_days,
        test_window_days=args.test_window_days,
        horizon_bars=args.horizon_bars,
        interval=args.interval,
    )
    if not splits:
        raise RuntimeError("No walk-forward splits available with current dataset and windows.")

    fold_results: list[FoldResult] = []
    oos_parts: list[pd.DataFrame] = []

    model = None
    for train_start, train_end, test_start, test_end in splits:
        train_df, test_df = build_fold_data(
            frame, feature_cols, train_start, train_end, test_start, test_end
        )
        if len(train_df) < 200 or len(test_df) < 20:
            continue

        params = {
            "objective": "regression",
            "metric": "l2",
            "learning_rate": args.learning_rate,
            "num_leaves": args.num_leaves,
            "feature_fraction": args.feature_fraction,
            "bagging_fraction": args.bagging_fraction,
            "bagging_freq": args.bagging_freq,
            "seed": args.seed,
            "verbosity": -1,
        }
        dtrain = lgb.Dataset(train_df[feature_cols], label=train_df["label"])
        model = lgb.train(params=params, train_set=dtrain, num_boost_round=args.num_boost_round)

        preds = model.predict(test_df[feature_cols])
        test_out = test_df[["datetime", "symbol", "label"]].copy()
        test_out["score"] = preds
        test_out["score_percentile"] = test_out.groupby("datetime")["score"].transform(compute_percentiles)
        oos_parts.append(test_out)

        ic = _pearson_corr(test_out["score"].to_numpy(), test_out["label"].to_numpy())
        rank_ic = _spearman_corr(test_out["score"].to_numpy(), test_out["label"].to_numpy())
        fold_results.append(
            FoldResult(
                train_start=str(train_start),
                train_end=str(train_end),
                test_start=str(test_start),
                test_end=str(test_end),
                train_rows=int(len(train_df)),
                test_rows=int(len(test_df)),
                ic=ic,
                rank_ic=rank_ic,
            )
        )

    if model is None or not fold_results:
        raise RuntimeError("Training produced no valid folds. Check dataset size/quality.")

    # Final fit on full usable data for deployment artifact.
    train_all = frame.dropna(subset=feature_cols + ["label"]).copy()
    params = {
        "objective": "regression",
        "metric": "l2",
        "learning_rate": args.learning_rate,
        "num_leaves": args.num_leaves,
        "feature_fraction": args.feature_fraction,
        "bagging_fraction": args.bagging_fraction,
        "bagging_freq": args.bagging_freq,
        "seed": args.seed,
        "verbosity": -1,
    }
    dtrain_all = lgb.Dataset(train_all[feature_cols], label=train_all["label"])
    final_model = lgb.train(params=params, train_set=dtrain_all, num_boost_round=args.num_boost_round)

    model_out = Path(args.model_out)
    model_out.parent.mkdir(parents=True, exist_ok=True)
    final_model.save_model(str(model_out))

    oos_frame = pd.concat(oos_parts, ignore_index=True) if oos_parts else pd.DataFrame()
    oos_ic = _pearson_corr(oos_frame["score"].to_numpy(), oos_frame["label"].to_numpy()) if not oos_frame.empty else float("nan")
    oos_rank_ic = _spearman_corr(oos_frame["score"].to_numpy(), oos_frame["label"].to_numpy()) if not oos_frame.empty else float("nan")
    pct_mean = float(oos_frame["score_percentile"].mean()) if not oos_frame.empty else float("nan")
    pct_std = float(oos_frame["score_percentile"].std(ddof=0)) if not oos_frame.empty else float("nan")

    report = {
        "dataset": str(dataset_path),
        "interval": args.interval,
        "horizon_bars": int(args.horizon_bars),
        "train_window_days": int(args.train_window_days),
        "test_window_days": int(args.test_window_days),
        "embargo_bars": int(args.horizon_bars),
        "purging": True,
        "feature_columns": feature_cols,
        "model_path": str(model_out),
        "folds": [f.__dict__ for f in fold_results],
        "oos_metrics": {
            "ic": oos_ic,
            "rank_ic": oos_rank_ic,
            "score_percentile_mean": pct_mean,
            "score_percentile_std": pct_std,
            "oos_rows": int(len(oos_frame)),
        },
    }
    return report


def main() -> int:
    args = parse_args()
    report = run_training(args)

    report_out = Path(args.report_out)
    report_out.parent.mkdir(parents=True, exist_ok=True)
    report_out.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(
        "trained_model="
        f"{args.model_out} "
        f"oos_ic={report['oos_metrics']['ic']:.6f} "
        f"oos_rank_ic={report['oos_metrics']['rank_ic']:.6f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
