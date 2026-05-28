"""Score the latest qlib bridge dataset and persist prediction outputs.

This script loads the prepared feature dataset, validates the feature manifest,
applies the trained LightGBM model to the most recent eligible rows, and stores
the resulting predictions, debug payloads, and readiness flags in SQLite.

It is the last mile of the qlib bridge workflow: it turns freshly refreshed
market data into model outputs that downstream strategy and execution jobs can
consume.
"""

from __future__ import annotations

import argparse
import json
import os
import sqlite3
import time
from pathlib import Path
from typing import Sequence

import lightgbm as lgb  # type: ignore[import-not-found]
import numpy as np
import pandas as pd

from features import FEATURE_COLS, add_features

_SCHEMA_READY_DB_PATHS: set[str] = set()


def _safe_int(value: object | None, default: int = 0) -> int:
    if value is None:
        return default
    if isinstance(value, int):
        return value
    try:
        s = str(value).strip()
        return int(s) if s else default
    except Exception:
        return default


def _safe_float(value: object | None, default: float = 0.0) -> float:
    if value is None:
        return default
    if isinstance(value, float):
        return value
    try:
        s = str(value).strip()
        return float(s) if s else default
    except Exception:
        return default


def _safe_str(value: object | None, default: str = "") -> str:
    if value is None:
        return default
    try:
        return str(value)
    except Exception:
        return default


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run latest Qlib-sidecar predictions and store into SQLite.")
    parser.add_argument("--dataset", required=True, help="Input csv/parquet dataset")
    parser.add_argument("--model-path", required=True, help="LightGBM text model path")
    parser.add_argument("--model-id", required=True, help="Model id key in sqlite")
    parser.add_argument("--run-id", required=False, default="", help="Active model run id")
    parser.add_argument("--horizon-bars", type=int, required=False, default=1)
    parser.add_argument("--interval", required=True, help="Signal interval, e.g. 1h")
    parser.add_argument("--db-path", default="data/qlib_predictions.db")
    parser.add_argument("--asof-ms", type=int, default=None, help="Specific candle open time (epoch ms)")
    parser.add_argument("--ready-dir", default="tmp/qlib_signals")
    parser.add_argument("--debug-json", default=None, help="Optional debug json output path")
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


def _to_epoch_ms(series: pd.Series) -> pd.Series:
    dt = pd.to_datetime(series, utc=True)
    dt_utc_naive = dt.dt.tz_convert("UTC").dt.tz_localize(None)
    # Keep conversion explicit to avoid deprecated tz-aware int casts on pandas 2.x+.
    return dt_utc_naive.astype("datetime64[ms]").view("int64").astype(np.int64)


def _feature_manifest_path(model_path: Path) -> Path:
    return model_path.with_suffix(model_path.suffix + ".features.json")


def validate_feature_manifest(model_path: Path, expected_cols: Sequence[str]) -> None:
    manifest_path = _feature_manifest_path(model_path)
    if not manifest_path.exists():
        raise RuntimeError(
            f"Missing model feature manifest: {manifest_path}. "
            "Retrain the model with current train_workflow.py to generate it."
        )

    raw = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise RuntimeError(f"Invalid manifest format in {manifest_path}")

    trained_cols = raw.get("feature_columns")
    if not isinstance(trained_cols, list) or not all(isinstance(item, str) for item in trained_cols):
        raise RuntimeError(f"Invalid feature_columns in {manifest_path}")

    expected = list(expected_cols)
    if trained_cols != expected:
        raise RuntimeError(
            "Model feature mismatch between training and inference. "
            f"trained={trained_cols} expected={expected}"
        )


def score_latest(
    frame: pd.DataFrame,
    booster: lgb.Booster,
    asof_ms: int | None,
) -> pd.DataFrame:
    scored = frame.dropna(subset=FEATURE_COLS).copy()
    if scored.empty:
        raise RuntimeError("No rows available after feature NA filtering.")

    scored["asof_open_time_ms"] = _to_epoch_ms(scored["datetime"])
    if asof_ms is None:
        asof_ms = int(scored["asof_open_time_ms"].max())

    latest = scored[scored["asof_open_time_ms"] == asof_ms].copy()
    if latest.empty:
        raise RuntimeError(f"No rows found for asof-ms={asof_ms}")

    latest["score"] = booster.predict(latest[FEATURE_COLS])
    latest = latest.sort_values("score", ascending=False).reset_index(drop=True)
    latest["rank"] = np.arange(1, len(latest) + 1, dtype=np.int64)
    latest["score_percentile"] = latest["score"].rank(method="average", pct=True, ascending=True).clip(0.0, 1.0)
    return latest


def init_schema(conn: sqlite3.Connection) -> None:
    conn.execute("PRAGMA journal_mode=WAL;")
    conn.execute("PRAGMA synchronous=NORMAL;")
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS qlib_predictions (
            model_id            TEXT    NOT NULL,
            run_id              TEXT,
            symbol              TEXT    NOT NULL,
            interval            TEXT    NOT NULL,
            asof_open_time_ms   INTEGER NOT NULL,
            generated_at_ms     INTEGER NOT NULL,
            horizon_bars        INTEGER NOT NULL,
            score               REAL    NOT NULL,
            rank                INTEGER,
            score_percentile    REAL,
            PRIMARY KEY (model_id, symbol, interval, asof_open_time_ms)
        );
        """
    )
    columns = {
        row[1]: row
        for row in conn.execute("PRAGMA table_info(qlib_predictions);").fetchall()
    }
    if "run_id" not in columns:
        conn.execute("ALTER TABLE qlib_predictions ADD COLUMN run_id TEXT;")
    if "horizon_bars" not in columns:
        conn.execute("ALTER TABLE qlib_predictions ADD COLUMN horizon_bars INTEGER NOT NULL DEFAULT 1;")
    conn.execute(
        """
        CREATE INDEX IF NOT EXISTS idx_qlib_pred_lookup
            ON qlib_predictions (model_id, interval, generated_at_ms DESC);
        """
    )


def upsert_predictions(
    db_path: Path,
    model_id: str,
    run_id: str,
    horizon_bars: int,
    interval: str,
    generated_at_ms: int,
    rows: pd.DataFrame,
) -> None:
    db_path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(db_path)
    try:
        db_key = str(db_path.resolve())
        if db_key not in _SCHEMA_READY_DB_PATHS:
            init_schema(conn)
            _SCHEMA_READY_DB_PATHS.add(db_key)
        payload: list[tuple[object, ...]] = []
        for row in rows.itertuples(index=False):
            payload.append(
                (
                    model_id,
                    run_id if run_id else None,
                    _safe_str(getattr(row, "symbol", None)),
                    interval,
                    _safe_int(getattr(row, "asof_open_time_ms", None)),
                    generated_at_ms,
                    _safe_int(horizon_bars),
                    _safe_float(getattr(row, "score", None)),
                    _safe_int(getattr(row, "rank", None)),
                    _safe_float(getattr(row, "score_percentile", None)),
                )
            )

        with conn:
            conn.executemany(
                """
                INSERT INTO qlib_predictions (
                    model_id, run_id, symbol, interval, asof_open_time_ms, generated_at_ms, horizon_bars, score, rank, score_percentile
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(model_id, symbol, interval, asof_open_time_ms)
                DO UPDATE SET
                    run_id = excluded.run_id,
                    generated_at_ms = excluded.generated_at_ms,
                    horizon_bars = excluded.horizon_bars,
                    score = excluded.score,
                    rank = excluded.rank,
                    score_percentile = excluded.score_percentile;
                """,
                payload,
            )
    finally:
        conn.close()


def write_debug_json(
    debug_path: Path,
    model_id: str,
    run_id: str,
    horizon_bars: int,
    interval: str,
    generated_at_ms: int,
    rows: pd.DataFrame,
) -> None:
    payload = {
        "generated_at_ms": generated_at_ms,
        "model_id": model_id,
        "run_id": run_id,
        "horizon_bars": horizon_bars,
        "interval": interval,
        "scores": [
            {
                "symbol": _safe_str(getattr(r, "symbol", None)),
                "score": _safe_float(getattr(r, "score", None)),
                "rank": _safe_int(getattr(r, "rank", None)),
                "score_percentile": _safe_float(getattr(r, "score_percentile", None)),
                "asof_open_time_ms": _safe_int(getattr(r, "asof_open_time_ms", None)),
            }
            for r in rows.itertuples(index=False)
        ],
    }
    debug_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = debug_path.with_suffix(debug_path.suffix + ".tmp")
    tmp_path.write_text(json.dumps(payload, ensure_ascii=True, separators=(",", ":")), encoding="utf-8")
    os.replace(tmp_path, debug_path)


def write_ready_flag(ready_dir: Path, generated_at_ms: int) -> Path:
    ready_dir.mkdir(parents=True, exist_ok=True)
    ready_path = ready_dir / f"ready_{generated_at_ms}.flag"
    ready_path.write_text("ok\n", encoding="utf-8")
    return ready_path


def explain_query_plan(db_path: Path, model_id: str, interval: str) -> list[tuple]:
    conn = sqlite3.connect(db_path)
    try:
        cursor = conn.execute(
            """
            EXPLAIN QUERY PLAN
            SELECT model_id, run_id, symbol, interval, asof_open_time_ms, generated_at_ms, horizon_bars, score, rank, score_percentile
            FROM qlib_predictions
            WHERE model_id = ? AND interval = ?
            ORDER BY generated_at_ms DESC
            LIMIT 50;
            """,
            (model_id, interval),
        )
        return list(cursor.fetchall())
    finally:
        conn.close()


def percentile_stats(values: Sequence[float]) -> dict[str, float]:
    arr = np.asarray(values, dtype=float)
    if arr.size == 0:
        return {"mean": float("nan"), "std": float("nan"), "p10": float("nan"), "p90": float("nan")}
    return {
        "mean": float(np.mean(arr)),
        "std": float(np.std(arr)),
        "p10": float(np.percentile(arr, 10)),
        "p90": float(np.percentile(arr, 90)),
    }


def main() -> int:
    args = parse_args()

    frame = load_dataset(Path(args.dataset))
    frame = add_features(frame)
    model_path = Path(args.model_path)
    validate_feature_manifest(model_path, FEATURE_COLS)
    booster = lgb.Booster(model_file=str(model_path))

    latest = score_latest(frame, booster, args.asof_ms)
    generated_at_ms = int(time.time() * 1000)

    db_path = Path(args.db_path)
    upsert_predictions(
        db_path=db_path,
        model_id=args.model_id,
        run_id=args.run_id,
        horizon_bars=max(1, int(args.horizon_bars)),
        interval=args.interval,
        generated_at_ms=generated_at_ms,
        rows=latest,
    )

    if args.debug_json:
        write_debug_json(
            debug_path=Path(args.debug_json),
            model_id=args.model_id,
            run_id=args.run_id,
            horizon_bars=max(1, int(args.horizon_bars)),
            interval=args.interval,
            generated_at_ms=generated_at_ms,
            rows=latest,
        )

    ready_flag = write_ready_flag(Path(args.ready_dir), generated_at_ms)
    query_plan = explain_query_plan(db_path, args.model_id, args.interval)

    stats = percentile_stats(latest["score_percentile"].astype(float).tolist())
    print(
        f"predictions={len(latest)} model_id={args.model_id} run_id={args.run_id or 'na'} "
        f"horizon_bars={max(1, int(args.horizon_bars))} interval={args.interval} "
        f"db={db_path} ready_flag={ready_flag}"
    )
    print(
        "score_percentile_stats "
        f"mean={stats['mean']:.6f} std={stats['std']:.6f} "
        f"p10={stats['p10']:.6f} p90={stats['p90']:.6f}"
    )
    print(f"query_plan={query_plan}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
