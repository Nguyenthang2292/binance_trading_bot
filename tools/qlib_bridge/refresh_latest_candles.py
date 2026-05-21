from __future__ import annotations

import argparse
import json
import sqlite3
import time
import urllib.parse
import urllib.request
from pathlib import Path

import pandas as pd


BINANCE_FAPI_KLINES = "https://fapi.binance.com/fapi/v1/klines"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Refresh exact latest closed candles for prediction asof.")
    parser.add_argument("--symbols", nargs="+", required=True)
    parser.add_argument("--interval", required=True)
    parser.add_argument("--asof-ms", type=int, required=True, help="Open time of just-closed candle")
    parser.add_argument("--dataset", required=True, help="CSV/Parquet dataset path used by predict_latest.py")
    parser.add_argument("--db-path", required=True, help="SQLite path for qlib_candles")
    parser.add_argument("--merge-mode", choices=["upsert"], default="upsert")
    parser.add_argument("--base-url", default=BINANCE_FAPI_KLINES)
    return parser.parse_args()


def interval_to_ms(interval: str) -> int:
    if len(interval) < 2:
        raise ValueError(f"unsupported interval: {interval}")
    unit = interval[-1]
    value = int(interval[:-1])
    if value <= 0:
        raise ValueError(f"unsupported interval: {interval}")
    if unit == "m":
        return value * 60 * 1000
    if unit == "h":
        return value * 60 * 60 * 1000
    if unit == "d":
        return value * 24 * 60 * 60 * 1000
    raise ValueError(f"unsupported interval: {interval}")


def fetch_exact_candle(base_url: str, symbol: str, interval: str, asof_ms: int) -> dict[str, object]:
    query = urllib.parse.urlencode(
        {
            "symbol": symbol,
            "interval": interval,
            "startTime": asof_ms,
            "endTime": asof_ms + interval_to_ms(interval) - 1,
            "limit": 1,
        }
    )
    req = urllib.request.Request(url=f"{base_url}?{query}", method="GET")
    with urllib.request.urlopen(req, timeout=30) as response:
        payload = json.loads(response.read())
    if not isinstance(payload, list) or not payload:
        raise RuntimeError(f"missing candle for {symbol} {interval} asof={asof_ms}")
    row = payload[0]
    if not isinstance(row, list) or len(row) < 9:
        raise RuntimeError(f"unexpected kline payload for {symbol}: {row!r}")
    open_time = int(row[0])
    close_time = int(row[6])
    if open_time != asof_ms:
        raise RuntimeError(
            f"unexpected open_time for {symbol}: got={open_time} expected={asof_ms}"
        )
    return {
        "datetime": pd.to_datetime(open_time, unit="ms", utc=True),
        "symbol": symbol,
        "open": float(row[1]),
        "high": float(row[2]),
        "low": float(row[3]),
        "close": float(row[4]),
        "volume": float(row[5]),
        "factor": 1.0,
        "quote_volume": float(row[7]),
        "trade_count": int(row[8]),
        "open_time_ms": open_time,
        "close_time_ms": close_time,
    }


def load_dataset(path: Path) -> pd.DataFrame:
    if not path.exists():
        return pd.DataFrame(
            columns=[
                "datetime",
                "symbol",
                "open",
                "high",
                "low",
                "close",
                "volume",
                "factor",
                "quote_volume",
                "trade_count",
            ]
        )
    if path.suffix.lower() == ".csv":
        frame = pd.read_csv(path)
    elif path.suffix.lower() == ".parquet":
        frame = pd.read_parquet(path)
    else:
        raise ValueError(f"unsupported dataset suffix: {path.suffix}")
    frame["datetime"] = pd.to_datetime(frame["datetime"], utc=True)
    return frame


def save_dataset(path: Path, frame: pd.DataFrame) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    ordered = frame.sort_values(["symbol", "datetime"], ascending=[True, True]).reset_index(drop=True)
    if path.suffix.lower() == ".csv":
        ordered.to_csv(path, index=False)
    elif path.suffix.lower() == ".parquet":
        ordered.to_parquet(path, index=False)
    else:
        raise ValueError(f"unsupported dataset suffix: {path.suffix}")


def upsert_dataset(path: Path, rows: list[dict[str, object]]) -> None:
    frame = load_dataset(path)
    latest = pd.DataFrame(rows)
    keep_cols = [c for c in frame.columns if c in latest.columns]
    if not keep_cols:
        keep_cols = [
            "datetime",
            "symbol",
            "open",
            "high",
            "low",
            "close",
            "volume",
            "factor",
            "quote_volume",
            "trade_count",
        ]
    frame = frame[keep_cols]
    latest = latest[keep_cols]
    merged = pd.concat([frame, latest], ignore_index=True)
    merged = merged.drop_duplicates(subset=["symbol", "datetime"], keep="last")
    save_dataset(path, merged)


def init_db(conn: sqlite3.Connection) -> None:
    conn.execute("PRAGMA journal_mode=WAL;")
    conn.execute("PRAGMA synchronous=NORMAL;")
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS qlib_candles (
            symbol            TEXT NOT NULL,
            interval          TEXT NOT NULL,
            open_time_ms      INTEGER NOT NULL,
            close_time_ms     INTEGER NOT NULL,
            open              REAL NOT NULL,
            high              REAL NOT NULL,
            low               REAL NOT NULL,
            close             REAL NOT NULL,
            volume            REAL NOT NULL,
            quote_volume      REAL,
            trade_count       INTEGER,
            inserted_at_ms    INTEGER NOT NULL,
            PRIMARY KEY (symbol, interval, open_time_ms)
        );
        """
    )


def upsert_candles(db_path: Path, interval: str, rows: list[dict[str, object]]) -> None:
    db_path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(db_path)
    try:
        init_db(conn)
        payload = [
            (
                str(r["symbol"]),
                interval,
                int(r["open_time_ms"]),
                int(r["close_time_ms"]),
                float(r["open"]),
                float(r["high"]),
                float(r["low"]),
                float(r["close"]),
                float(r["volume"]),
                float(r["quote_volume"]),
                int(r["trade_count"]),
                int(time.time() * 1000),
            )
            for r in rows
        ]
        with conn:
            conn.executemany(
                """
                INSERT INTO qlib_candles(
                    symbol, interval, open_time_ms, close_time_ms,
                    open, high, low, close, volume, quote_volume, trade_count, inserted_at_ms
                ) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(symbol, interval, open_time_ms)
                DO UPDATE SET
                    close_time_ms = excluded.close_time_ms,
                    open = excluded.open,
                    high = excluded.high,
                    low = excluded.low,
                    close = excluded.close,
                    volume = excluded.volume,
                    quote_volume = excluded.quote_volume,
                    trade_count = excluded.trade_count,
                    inserted_at_ms = excluded.inserted_at_ms;
                """,
                payload,
            )
    finally:
        conn.close()


def main() -> int:
    args = parse_args()
    symbols = [s.upper() for s in args.symbols]
    rows = [fetch_exact_candle(args.base_url, s, args.interval, args.asof_ms) for s in symbols]

    upsert_dataset(Path(args.dataset), rows)
    upsert_candles(Path(args.db_path), args.interval, rows)

    print(
        f"refreshed_rows={len(rows)} interval={args.interval} asof_ms={args.asof_ms} "
        f"dataset={args.dataset} db={args.db_path}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
