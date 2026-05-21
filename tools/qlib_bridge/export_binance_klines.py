from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import warnings
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable

import pandas as pd


BINANCE_FAPI_KLINES = "https://fapi.binance.com/fapi/v1/klines"

OUTPUT_COLUMNS = [
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

FEATURE_FIELDS = ["$open", "$high", "$low", "$close", "$volume"]

INTERVAL_TO_QLIB_FREQ = {
    "1d": "day",
    "4h": "240min",
    "1h": "60min",
    "30m": "30min",
}


@dataclass(frozen=True)
class ExportConfig:
    symbols: list[str]
    interval: str
    start_ms: int
    end_ms: int
    output_path: Path
    request_limit: int
    request_pause_ms: int
    base_url: str


def _to_utc_iso(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000.0, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


def _read_json(url: str, timeout_s: int = 30) -> object:
    req = urllib.request.Request(url=url, method="GET")
    with urllib.request.urlopen(req, timeout=timeout_s) as response:
        payload = response.read()
    return json.loads(payload)


def fetch_binance_klines(cfg: ExportConfig) -> pd.DataFrame:
    rows: list[dict[str, object]] = []
    for symbol in cfg.symbols:
        start_ms = cfg.start_ms
        while start_ms <= cfg.end_ms:
            query = urllib.parse.urlencode(
                {
                    "symbol": symbol,
                    "interval": cfg.interval,
                    "startTime": start_ms,
                    "endTime": cfg.end_ms,
                    "limit": cfg.request_limit,
                }
            )
            url = f"{cfg.base_url}?{query}"
            batch = _read_json(url)
            if not isinstance(batch, list):
                raise RuntimeError(f"Unexpected response payload for {symbol}: {batch!r}")
            if not batch:
                break

            for item in batch:
                if not isinstance(item, list) or len(item) < 11:
                    continue
                open_time_ms = int(item[0])
                rows.append(
                    {
                        "datetime": _to_utc_iso(open_time_ms),
                        "symbol": symbol,
                        "open": float(item[1]),
                        "high": float(item[2]),
                        "low": float(item[3]),
                        "close": float(item[4]),
                        "volume": float(item[5]),
                        "factor": 1.0,
                        "quote_volume": float(item[7]),
                        "trade_count": int(item[8]),
                    }
                )

            last_open_time_ms = int(batch[-1][0])
            if last_open_time_ms <= start_ms:
                break
            start_ms = last_open_time_ms + 1
            if cfg.request_pause_ms > 0:
                time.sleep(cfg.request_pause_ms / 1000.0)

    if not rows:
        raise RuntimeError("No kline rows fetched. Check symbols/interval/date range.")

    frame = pd.DataFrame(rows, columns=OUTPUT_COLUMNS)
    frame = frame.sort_values(["symbol", "datetime"], ascending=[True, True]).reset_index(drop=True)
    return frame


def write_dataset(frame: pd.DataFrame, output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    suffix = output_path.suffix.lower()
    if suffix == ".csv":
        frame.to_csv(output_path, index=False)
    elif suffix == ".parquet":
        frame.to_parquet(output_path, index=False)
    else:
        raise ValueError(f"Unsupported output suffix: {suffix}. Use .csv or .parquet")


def _safe_freq_string(interval: str) -> str:
    mapped = INTERVAL_TO_QLIB_FREQ.get(interval)
    if mapped:
        return mapped
    return interval


def run_dump_bin(
    dump_bin_script: Path,
    data_path: Path,
    qlib_dir: Path,
    interval: str,
    file_suffix: str,
    incremental: bool,
) -> None:
    if not dump_bin_script.exists():
        raise FileNotFoundError(f"dump_bin.py not found: {dump_bin_script}")
    qlib_dir.mkdir(parents=True, exist_ok=True)

    mode = "dump_update" if incremental else "dump_all"
    cmd = [
        sys.executable,
        str(dump_bin_script),
        mode,
        "--data_path",
        str(data_path),
        "--qlib_dir",
        str(qlib_dir),
        "--freq",
        _safe_freq_string(interval),
        "--date_field_name",
        "datetime",
        "--symbol_field_name",
        "symbol",
        "--include_fields",
        ",".join(["open", "high", "low", "close", "volume", "factor", "quote_volume", "trade_count"]),
        "--file_suffix",
        file_suffix,
    ]
    subprocess.run(cmd, check=True)


def prepare_dump_all_input(frame: pd.DataFrame, output_path: Path) -> Path:
    """
    dump_bin dump_all determines symbol from file name, not from symbol column.
    For full rebuild we must split into one file per symbol.
    """
    suffix = output_path.suffix.lower()
    if suffix not in {".csv", ".parquet"}:
        raise ValueError(f"Unsupported output suffix for dump_all preparation: {suffix}")

    split_dir = output_path.parent / f"{output_path.stem}_by_symbol"
    split_dir.mkdir(parents=True, exist_ok=True)

    for symbol, part in frame.groupby("symbol", sort=True):
        symbol_file = split_dir / f"{symbol}{suffix}"
        if suffix == ".csv":
            part.to_csv(symbol_file, index=False)
        else:
            part.to_parquet(symbol_file, index=False)
    return split_dir


def _floor_to_bar(dt: pd.Timestamp, interval: str) -> pd.Timestamp:
    if dt.tzinfo is not None:
        dt = dt.tz_convert("UTC").tz_localize(None)
    if interval == "1d":
        return dt.floor("D")
    if interval == "4h":
        return dt.floor("4h")
    if interval == "1h":
        return dt.floor("h")
    if interval == "30m":
        return dt.floor("30min")
    return dt


def _interval_to_timedelta(interval: str) -> pd.Timedelta:
    if interval.endswith("m"):
        return pd.Timedelta(minutes=int(interval[:-1]))
    if interval.endswith("h"):
        return pd.Timedelta(hours=int(interval[:-1]))
    if interval.endswith("d"):
        return pd.Timedelta(days=int(interval[:-1]))
    raise ValueError(f"Unsupported interval for continuity checks: {interval}")


def _check_continuity(frame: pd.DataFrame, interval: str) -> None:
    step = _interval_to_timedelta(interval)
    frame = frame.copy()
    frame["datetime"] = pd.to_datetime(frame["datetime"], utc=True)

    for symbol, part in frame.groupby("symbol"):
        part = part.sort_values("datetime")
        diffs = part["datetime"].diff().dropna()
        if not diffs.empty:
            broken = diffs[diffs != step]
            if not broken.empty:
                raise RuntimeError(f"Continuity check failed for {symbol}: found {len(broken)} non-{step} gaps")

        midnight_rows = part[part["datetime"].dt.hour == 0]
        if midnight_rows.empty:
            raise RuntimeError(f"Midnight continuity check failed for {symbol}: no 00:00 UTC rows found")

        sunday_to_monday = 0
        timestamps = part["datetime"].tolist()
        for i in range(1, len(timestamps)):
            prev_dt = timestamps[i - 1]
            cur_dt = timestamps[i]
            if prev_dt.weekday() == 6 and cur_dt.weekday() == 0 and (cur_dt - prev_dt) == step:
                sunday_to_monday += 1
        if sunday_to_monday == 0:
            span = (part["datetime"].max() - part["datetime"].min()).days
            if span >= 7:
                raise RuntimeError(f"Sunday-to-Monday continuity check failed for {symbol}")
            warnings.warn(
                f"Sunday-to-Monday continuity skipped for {symbol}: dataset span {span}d < 7d",
                stacklevel=2,
            )


def register_crypto_24_7_calendar(timestamps: Iterable[pd.Timestamp], interval: str) -> None:
    """
    Best-effort runtime patch: register a 24/7 calendar before calling qlib D.features().
    Qlib APIs differ by version, so this patch intentionally falls back silently when
    internals are not compatible.
    """

    try:
        import qlib.data as qlib_data  # type: ignore
    except Exception:
        return

    ts_values = sorted({_floor_to_bar(ts, interval) for ts in timestamps})
    if not ts_values:
        return
    calendar = pd.DatetimeIndex(ts_values)
    freq = _safe_freq_string(interval)

    original_calendar = getattr(qlib_data.D, "calendar", None)
    if original_calendar is None:
        return

    def patched_calendar(start_time=None, end_time=None, freq_param=freq, future=False):  # noqa: ANN001
        target_freq = freq_param or freq
        if str(target_freq) != str(freq):
            return original_calendar(start_time=start_time, end_time=end_time, freq=freq_param, future=future)
        out = calendar
        if start_time is not None:
            out = out[out >= pd.Timestamp(start_time)]
        if end_time is not None:
            out = out[out <= pd.Timestamp(end_time)]
        return out

    setattr(qlib_data.D, "calendar", patched_calendar)


def smoke_test_qlib_features(
    frame: pd.DataFrame,
    interval: str,
    qlib_provider_uri: str | None,
) -> None:
    _check_continuity(frame, interval)
    if not qlib_provider_uri:
        return

    try:
        import qlib  # type: ignore
        import qlib.data as qlib_data  # type: ignore
    except Exception as exc:
        raise RuntimeError("Qlib is required for --smoke-test-qlib but import failed") from exc

    qlib.init(provider_uri=qlib_provider_uri, region="cn")
    dt_index = pd.to_datetime(frame["datetime"], utc=True)
    register_crypto_24_7_calendar(dt_index, interval)

    instruments = sorted(frame["symbol"].unique().tolist())
    start_time = dt_index.min().tz_convert("UTC").tz_localize(None).to_pydatetime()
    end_time = dt_index.max().tz_convert("UTC").tz_localize(None).to_pydatetime()
    feature_df = qlib_data.D.features(instruments, FEATURE_FIELDS, start_time=start_time, end_time=end_time, freq=_safe_freq_string(interval))

    if feature_df.empty:
        raise RuntimeError("Qlib smoke test failed: D.features returned empty frame")

    missing_cols = [col for col in FEATURE_FIELDS if col not in feature_df.columns]
    if missing_cols:
        raise RuntimeError(f"Qlib smoke test failed: missing feature columns {missing_cols}")

    if feature_df[FEATURE_FIELDS].isna().any().any():
        raise RuntimeError("Qlib smoke test failed: OHLCV contains NaN")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Export Binance Futures klines for Qlib bridge.")
    parser.add_argument("--symbols", nargs="+", required=True, help="Symbols, e.g. BTCUSDT ETHUSDT")
    parser.add_argument("--interval", required=True, help="Binance interval, e.g. 1h, 30m, 4h, 1d")
    parser.add_argument("--start-ms", type=int, required=True, help="UTC start time (epoch milliseconds)")
    parser.add_argument("--end-ms", type=int, default=None)
    parser.add_argument("--output", required=True, help="Output .csv or .parquet path")
    parser.add_argument("--request-limit", type=int, default=1000, help="Per-request kline limit")
    parser.add_argument("--request-pause-ms", type=int, default=150, help="Pause between API requests")
    parser.add_argument("--base-url", default=BINANCE_FAPI_KLINES, help="Binance klines endpoint")
    parser.add_argument("--smoke-test-qlib", action="store_true", help="Run continuity and Qlib D.features smoke test")
    parser.add_argument("--qlib-provider-uri", default=None, help="Qlib provider URI for smoke test")
    parser.add_argument("--dump-bin-script", default=None, help="Path to qlib/scripts/dump_bin.py")
    parser.add_argument("--qlib-dir", default=None, help="Output qlib binary directory")
    parser.add_argument(
        "--convert-mode",
        choices=["none", "full", "incremental"],
        default="none",
        help="Qlib binary conversion mode via dump_bin.py",
    )
    return parser.parse_args()


def _resolve_end_ms(raw_value: object) -> int:
    if isinstance(raw_value, int):
        return raw_value
    return int(time.time() * 1000)


def main() -> int:
    args = parse_args()
    output_path = Path(args.output)
    cfg = ExportConfig(
        symbols=[str(s).upper() for s in args.symbols],
        interval=str(args.interval),
        start_ms=int(args.start_ms),
        end_ms=_resolve_end_ms(args.end_ms),
        output_path=output_path,
        request_limit=int(args.request_limit),
        request_pause_ms=int(args.request_pause_ms),
        base_url=str(args.base_url),
    )
    if cfg.end_ms <= cfg.start_ms:
        raise ValueError("end-ms must be greater than start-ms")

    frame = fetch_binance_klines(cfg)
    write_dataset(frame, cfg.output_path)

    if args.convert_mode != "none":
        if not args.dump_bin_script:
            raise ValueError("--dump-bin-script is required when convert-mode is not none")
        if not args.qlib_dir:
            raise ValueError("--qlib-dir is required when convert-mode is not none")
        dump_data_path = cfg.output_path.parent
        if args.convert_mode == "full":
            dump_data_path = prepare_dump_all_input(frame, cfg.output_path)
        run_dump_bin(
            dump_bin_script=Path(args.dump_bin_script),
            data_path=dump_data_path,
            qlib_dir=Path(args.qlib_dir),
            interval=cfg.interval,
            file_suffix=cfg.output_path.suffix.lower(),
            incremental=args.convert_mode == "incremental",
        )

    if args.smoke_test_qlib:
        smoke_test_qlib_features(frame, cfg.interval, args.qlib_provider_uri)

    print(f"exported_rows={len(frame)} symbols={len(cfg.symbols)} output={cfg.output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
