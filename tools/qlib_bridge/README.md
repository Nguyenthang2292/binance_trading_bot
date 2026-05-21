# Qlib Bridge

This folder hosts the Python sidecar for Microsoft Qlib integration.

## Directory

```text
tools/qlib_bridge/
  export_binance_klines.py
  train_workflow.py
  predict_latest.py
  requirements.txt
  README.md
```

## Environment Setup (Windows)

Use an isolated virtual environment similar to the Gemini sidecar pattern.

```powershell
python -m venv .venv-qlib
.\.venv-qlib\Scripts\python.exe -m pip install --upgrade pip
.\.venv-qlib\Scripts\python.exe -m pip install -r tools\qlib_bridge\requirements.txt
```

Recommended runtime settings:

- Interpreter path: `.venv-qlib\Scripts\python.exe`
- Working directory: repository root
- Runtime artifacts:
  - SQLite: `data/qlib_predictions.db`
  - Readiness flags: `tmp/qlib_signals/`

## Quick Start

Export Binance OHLCV:

```powershell
.\.venv-qlib\Scripts\python.exe tools\qlib_bridge\export_binance_klines.py `
  --symbols BTCUSDT ETHUSDT `
  --interval 1h `
  --start-ms 1714521600000 `
  --output data\qlib_csv\binance_1h.parquet
```

Train baseline walk-forward model:

```powershell
.\.venv-qlib\Scripts\python.exe tools\qlib_bridge\train_workflow.py `
  --dataset data\qlib_csv\binance_1h.parquet `
  --interval 1h `
  --horizon-bars 6 `
  --model-out data\qlib\models\lightgbm_1h_v1.txt `
  --report-out data\qlib\reports\lightgbm_1h_v1.json
```

Generate latest predictions to SQLite:

```powershell
.\.venv-qlib\Scripts\python.exe tools\qlib_bridge\predict_latest.py `
  --dataset data\qlib_csv\binance_1h.parquet `
  --model-path data\qlib\models\lightgbm_1h_v1.txt `
  --model-id lightgbm_1h_v1 `
  --interval 1h `
  --db-path data\qlib_predictions.db
```

## Notes

- Primary contract is SQLite (`qlib_predictions` table). JSON output is debug-only.
- SQLite runs in WAL mode for safe atomic writes on Windows.
- If debug JSON is enabled, file replacement uses `os.replace()` for atomic overwrite.
- `predict_latest.py` writes readiness flags: `tmp/qlib_signals/ready_<epoch_ms>.flag`.
