from __future__ import annotations

import argparse
import hashlib
import importlib
import json
import sqlite3
import sys
import time
import uuid
from typing import Any

SCHEMA_VERSION = 7

ALLOWED_CLASSES = frozenset(
    {
        "qlib.contrib.strategy.TopkDropoutStrategy",
        "qlib.contrib.strategy.SoftTopkStrategy",
        "qlib.contrib.strategy.rule_strategy.FileOrderStrategy",
    }
)

CLASS_ALIASES = {
    "TopkDropoutStrategy": "qlib.contrib.strategy.TopkDropoutStrategy",
    "SoftTopkStrategy": "qlib.contrib.strategy.SoftTopkStrategy",
    "FileOrderStrategy": "qlib.contrib.strategy.rule_strategy.FileOrderStrategy",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--db-path", default="data/qlib_predictions.db")
    parser.add_argument("--strategy-id", required=True)
    parser.add_argument("--qlib-class", required=True)
    parser.add_argument("--model-id", required=True)
    parser.add_argument("--interval", required=True)
    parser.add_argument("--config-json", default="{}")
    parser.add_argument("--universe-hash", default="default")
    return parser.parse_args()


def normalize_class_name(value: str) -> str:
    return CLASS_ALIASES.get(value, value)


def try_load_qlib_class(class_path: str) -> type[Any] | None:
    module_name, class_name = class_path.rsplit(".", 1)
    try:
        module = importlib.import_module(module_name)
    except ImportError:
        return None
    loaded = getattr(module, class_name, None)
    return loaded if isinstance(loaded, type) else None


def configure_connection(db: sqlite3.Connection) -> None:
    db.execute("PRAGMA journal_mode = WAL;")
    db.execute("PRAGMA busy_timeout = 5000;")
    db.execute("PRAGMA foreign_keys = ON;")
    db.execute("PRAGMA synchronous = NORMAL;")


def table_columns(db: sqlite3.Connection, table: str) -> set[str]:
    return {row[1] for row in db.execute(f"PRAGMA table_info({table});").fetchall()}


def ensure_column(db: sqlite3.Connection, table: str, name: str, definition: str) -> None:
    if name not in table_columns(db, table):
        db.execute(f"ALTER TABLE {table} ADD COLUMN {name} {definition};")


def ensure_user_version(db: sqlite3.Connection) -> None:
    row = db.execute("PRAGMA user_version;").fetchone()
    current = int(row[0]) if row else 0
    if current not in (0, SCHEMA_VERSION):
        raise RuntimeError(f"schema version mismatch: expected {SCHEMA_VERSION}, got {current}")
    if current == 0:
        db.execute(f"PRAGMA user_version = {SCHEMA_VERSION};")


def create_tables_if_needed(db: sqlite3.Connection) -> None:
    ensure_user_version(db)
    db.execute(
        """
        CREATE TABLE IF NOT EXISTS qlib_strategy_runs (
            run_id              TEXT PRIMARY KEY,
            strategy_id         TEXT NOT NULL,
            qlib_class          TEXT NOT NULL,
            config_hash         TEXT NOT NULL,
            model_id            TEXT,
            model_run_id        TEXT,
            interval            TEXT NOT NULL,
            universe_hash       TEXT NOT NULL,
            started_at_ms       INTEGER NOT NULL,
            completed_at_ms     INTEGER,
            status              TEXT NOT NULL CHECK (status IN ('running','succeeded','failed')),
            error               TEXT
        );
        """
    )
    db.execute(
        """
        CREATE TABLE IF NOT EXISTS qlib_strategy_targets (
            strategy_id         TEXT NOT NULL,
            run_id              TEXT NOT NULL,
            symbol              TEXT NOT NULL,
            interval            TEXT NOT NULL,
            target_weight       REAL NOT NULL,
            generated_at_ms     INTEGER NOT NULL,
            PRIMARY KEY (strategy_id, symbol, interval, run_id),
            FOREIGN KEY (run_id) REFERENCES qlib_strategy_runs(run_id)
        );
        """
    )
    db.execute(
        """
        CREATE TABLE IF NOT EXISTS qlib_strategy_decisions (
            strategy_id         TEXT NOT NULL,
            run_id              TEXT NOT NULL,
            model_id            TEXT,
            model_run_id        TEXT,
            symbol              TEXT NOT NULL,
            interval            TEXT NOT NULL,
            asof_open_time_ms   INTEGER NOT NULL,
            generated_at_ms     INTEGER NOT NULL,
            action              TEXT NOT NULL CHECK (action IN ('buy','hold','none')),
            direction           TEXT NOT NULL CHECK (direction IN ('long','none')),
            target_weight       REAL,
            score               REAL,
            score_percentile    REAL,
            confidence          REAL NOT NULL,
            reason              TEXT,
            PRIMARY KEY (strategy_id, symbol, interval, asof_open_time_ms),
            FOREIGN KEY (run_id) REFERENCES qlib_strategy_runs(run_id)
        );
        """
    )
    db.execute(
        """
        CREATE TABLE IF NOT EXISTS qlib_adapter_runtime_state (
            adapter_id          TEXT NOT NULL,
            interval            TEXT NOT NULL,
            execution_mode      TEXT NOT NULL CHECK (
                execution_mode IN ('disabled','shadow','shadow_only','live_canary','live')
            ),
            promotion_profile   TEXT NOT NULL DEFAULT 'default',
            active_run_id       TEXT,
            state_version       INTEGER NOT NULL DEFAULT 0,
            promoted_at_ms      INTEGER,
            promoted_by         TEXT,
            last_decision_at_ms INTEGER,
            last_failure_at_ms  INTEGER,
            last_failure_reason TEXT,
            updated_at_ms       INTEGER NOT NULL,
            rollback_reason     TEXT,
            PRIMARY KEY (adapter_id, interval)
        );
        """
    )
    ensure_column(db, "qlib_adapter_runtime_state", "promotion_profile", "TEXT NOT NULL DEFAULT 'default'")
    ensure_column(db, "qlib_adapter_runtime_state", "promoted_at_ms", "INTEGER")
    ensure_column(db, "qlib_adapter_runtime_state", "promoted_by", "TEXT")
    ensure_column(db, "qlib_adapter_runtime_state", "last_decision_at_ms", "INTEGER")
    ensure_column(db, "qlib_adapter_runtime_state", "last_failure_at_ms", "INTEGER")
    ensure_column(db, "qlib_adapter_runtime_state", "last_failure_reason", "TEXT")
    db.execute(
        """
        CREATE TABLE IF NOT EXISTS qlib_promotion_profiles (
            profile_name        TEXT PRIMARY KEY,
            qlib_class          TEXT NOT NULL,
            profile_json        TEXT NOT NULL,
            updated_at_ms       INTEGER NOT NULL
        );
        """
    )
    db.execute(
        """
        CREATE TABLE IF NOT EXISTS qlib_execution_requests (
            request_id          TEXT PRIMARY KEY,
            symbol              TEXT NOT NULL,
            side                TEXT NOT NULL,
            quantity            TEXT NOT NULL,
            position_side       TEXT NOT NULL,
            metadata_json       TEXT,
            status              TEXT NOT NULL CHECK (status IN ('pending','succeeded','expired','failed')),
            created_at_ms       INTEGER NOT NULL,
            deadline_ms         INTEGER NOT NULL,
            error               TEXT
        );
        """
    )
    db.execute(
        """
        CREATE TABLE IF NOT EXISTS qlib_execution_plans (
            plan_id             TEXT PRIMARY KEY,
            request_id          TEXT NOT NULL,
            algorithm           TEXT NOT NULL,
            status              TEXT NOT NULL CHECK (status IN ('running','succeeded','failed','expired')),
            generated_at_ms     INTEGER NOT NULL,
            expires_at_ms       INTEGER NOT NULL,
            total_quantity      TEXT NOT NULL,
            slice_count         INTEGER NOT NULL,
            error               TEXT,
            FOREIGN KEY (request_id) REFERENCES qlib_execution_requests(request_id)
        );
        """
    )
    db.execute(
        """
        CREATE TABLE IF NOT EXISTS qlib_execution_slices (
            slice_id            TEXT PRIMARY KEY,
            plan_id             TEXT NOT NULL,
            slice_index         INTEGER NOT NULL,
            due_at_ms           INTEGER NOT NULL,
            side                TEXT NOT NULL,
            quantity            TEXT NOT NULL,
            status              TEXT NOT NULL CHECK (
                status IN ('pending','submitted','filled','failed','revoked')
            ),
            revoked_at_ms       INTEGER,
            revoke_reason       TEXT,
            FOREIGN KEY (plan_id) REFERENCES qlib_execution_plans(plan_id)
        );
        """
    )


def prediction_run_column(db: sqlite3.Connection) -> str | None:
    columns = table_columns(db, "qlib_predictions")
    if "run_id" in columns:
        return "run_id"
    if "model_run_id" in columns:
        return "model_run_id"
    return None


def confidence_from_percentile(score_percentile: float | None) -> float:
    if score_percentile is None:
        return 0.0
    value = float(score_percentile)
    if value > 1.0:
        value /= 100.0
    return min(max(value, 0.0), 1.0)


def fetch_latest_predictions(
    db: sqlite3.Connection,
    model_id: str,
    interval: str,
) -> list[dict[str, Any]]:
    run_col = prediction_run_column(db)
    select_run = f", {run_col}" if run_col else ", NULL"
    latest_row = db.execute(
        "SELECT MAX(generated_at_ms) FROM qlib_predictions WHERE model_id = ? AND interval = ?",
        (model_id, interval),
    ).fetchone()
    if not latest_row or latest_row[0] is None:
        return []

    cursor = db.execute(
        f"""
        SELECT symbol, asof_open_time_ms, score, score_percentile{select_run}
        FROM qlib_predictions
        WHERE model_id = ? AND interval = ? AND generated_at_ms = ?
        ORDER BY score DESC
        """,
        (model_id, interval, latest_row[0]),
    )
    return [
        {
            "symbol": symbol,
            "asof_open_time_ms": asof_open_time_ms,
            "score": score,
            "score_percentile": score_percentile,
            "model_run_id": model_run_id or "",
        }
        for symbol, asof_open_time_ms, score, score_percentile, model_run_id in cursor.fetchall()
    ]


def latest_target_weights(db: sqlite3.Connection, strategy_id: str, interval: str) -> dict[str, float]:
    latest = db.execute(
        """
        SELECT run_id
        FROM qlib_strategy_runs
        WHERE strategy_id = ? AND interval = ? AND status = 'succeeded'
        ORDER BY completed_at_ms DESC, started_at_ms DESC
        LIMIT 1
        """,
        (strategy_id, interval),
    ).fetchone()
    if not latest:
        return {}
    return {
        symbol: float(weight)
        for symbol, weight in db.execute(
            """
            SELECT symbol, target_weight
            FROM qlib_strategy_targets
            WHERE strategy_id = ? AND interval = ? AND run_id = ?
            """,
            (strategy_id, interval, latest[0]),
        ).fetchall()
    }


def materialize_decisions(
    predictions: list[dict[str, Any]],
    selected_weights: dict[str, float],
    qlib_class: str,
    reason_suffix: str,
) -> list[dict[str, Any]]:
    by_symbol = {row["symbol"]: row for row in predictions}
    decisions = []
    for symbol, target_weight in selected_weights.items():
        row = by_symbol.get(symbol)
        if row is None:
            continue
        decisions.append(
            {
                "symbol": symbol,
                "asof_open_time_ms": row["asof_open_time_ms"],
                "score": row["score"],
                "score_percentile": row["score_percentile"],
                "action": "buy",
                "direction": "long",
                "target_weight": target_weight,
                "confidence": confidence_from_percentile(row.get("score_percentile")),
                "reason": f"qlib_class={qlib_class} {reason_suffix}",
                "model_run_id": row.get("model_run_id", ""),
            }
        )
    decisions.sort(key=lambda item: item.get("score") or 0.0, reverse=True)
    return decisions


def run_topk_dropout(
    db: sqlite3.Connection,
    args: argparse.Namespace,
    config: dict[str, Any],
    qlib_class: str,
) -> list[dict[str, Any]]:
    topk = max(int(config.get("topk", config.get("k", 5))), 1)
    n_drop = max(int(config.get("n_drop", topk)), 0)
    predictions = fetch_latest_predictions(db, args.model_id, args.interval)
    if not predictions:
        return []

    ranked_symbols = [row["symbol"] for row in predictions]
    ranked_set = set(ranked_symbols)
    previous = latest_target_weights(db, args.strategy_id, args.interval)
    if not previous:
        selected = ranked_symbols[:topk]
    else:
        current = [symbol for symbol in previous if symbol in ranked_set]
        last_by_score = [symbol for symbol in ranked_symbols if symbol in current]
        today = [symbol for symbol in ranked_symbols if symbol not in set(last_by_score)]
        combined = [symbol for symbol in ranked_symbols if symbol in set(last_by_score).union(today[: n_drop + topk - len(last_by_score)])]
        sell = [symbol for symbol in reversed(combined) if symbol in set(last_by_score)][:n_drop]
        sell_set = set(sell)
        keep = [symbol for symbol in last_by_score if symbol not in sell_set]
        buy_count = max(0, len(sell) + topk - len(last_by_score))
        buy = today[:buy_count]
        selected = (keep + buy)[:topk]
        if len(selected) < topk:
            selected_set = set(selected)
            selected.extend(symbol for symbol in ranked_symbols if symbol not in selected_set)
            selected = selected[:topk]

    weight = 1.0 / max(len(selected), 1)
    return materialize_decisions(
        predictions,
        {symbol: weight for symbol in selected},
        qlib_class,
        f"topk_dropout topk={topk} n_drop={n_drop}",
    )


def run_soft_topk(
    db: sqlite3.Connection,
    args: argparse.Namespace,
    config: dict[str, Any],
    qlib_class: str,
) -> list[dict[str, Any]]:
    topk = max(int(config.get("topk", config.get("k", 5))), 1)
    predictions = fetch_latest_predictions(db, args.model_id, args.interval)
    if not predictions:
        return []

    top_symbols = [row["symbol"] for row in predictions[:topk]]
    buy_signal = set(top_symbols)
    previous = latest_target_weights(db, args.strategy_id, args.interval)
    if not previous:
        weights = {symbol: 1.0 / topk for symbol in top_symbols}
    else:
        max_sold_weight = float(config.get("max_sold_weight", config.get("trade_impact_limit", 1.0)))
        max_sold_weight = min(max(max_sold_weight, 0.0), 1.0)
        buy_method = str(config.get("buy_method", "first_fill"))
        weights = dict(previous)
        sold_weight = 0.0
        for symbol in list(weights):
            if symbol not in buy_signal:
                sold = min(max_sold_weight, weights[symbol])
                sold_weight += sold
                weights[symbol] -= sold
        if buy_method == "average_fill":
            add = sold_weight / len(buy_signal)
            for symbol in top_symbols:
                weights[symbol] = weights.get(symbol, 0.0) + add
        else:
            for symbol in top_symbols:
                add = min(max((1.0 / topk) - weights.get(symbol, 0.0), 0.0), sold_weight)
                weights[symbol] = weights.get(symbol, 0.0) + add
                sold_weight -= add
                if sold_weight <= 0:
                    break
        weights = {
            symbol: weight
            for symbol, weight in weights.items()
            if weight > 0 and symbol in {row["symbol"] for row in predictions}
        }

    return materialize_decisions(
        predictions,
        weights,
        qlib_class,
        f"soft_topk topk={topk} max_sold_weight={float(config.get('max_sold_weight', config.get('trade_impact_limit', 1.0)))}",
    )


def run_strategy_policy(db: sqlite3.Connection, args: argparse.Namespace, config: dict[str, Any], qlib_class: str) -> list[dict[str, Any]]:
    if qlib_class.endswith("TopkDropoutStrategy"):
        return run_topk_dropout(db, args, config, qlib_class)
    if qlib_class.endswith("SoftTopkStrategy"):
        return run_soft_topk(db, args, config, qlib_class)
    if qlib_class.endswith("FileOrderStrategy"):
        return list(config.get("orders", []))
    return []


def upsert_promotion_profile(db: sqlite3.Connection, config: dict[str, Any], qlib_class: str, now_ms_value: int) -> None:
    profile_name = config.get("promotion_profile")
    profile_body = config.get("promotion_profile_config")
    if not profile_name or not isinstance(profile_name, str):
        return
    payload = profile_body if isinstance(profile_body, dict) else {}
    db.execute(
        """
        INSERT INTO qlib_promotion_profiles(profile_name, qlib_class, profile_json, updated_at_ms)
        VALUES(?, ?, ?, ?)
        ON CONFLICT(profile_name) DO UPDATE SET
            qlib_class = excluded.qlib_class,
            profile_json = excluded.profile_json,
            updated_at_ms = excluded.updated_at_ms
        """,
        (profile_name, qlib_class, json.dumps(payload, sort_keys=True), now_ms_value),
    )


def main(args: argparse.Namespace) -> int:
    qlib_class = normalize_class_name(args.qlib_class)
    if qlib_class not in ALLOWED_CLASSES:
        print(f"Error: qlib_class '{args.qlib_class}' is not in allowed list.", file=sys.stderr)
        return 1

    config = json.loads(args.config_json)
    loaded_class = try_load_qlib_class(qlib_class)
    if loaded_class is not None:
        config["_qlib_resolved_class"] = f"{loaded_class.__module__}.{loaded_class.__name__}"

    config_hash = hashlib.sha256(json.dumps(config, sort_keys=True).encode("utf-8")).hexdigest()
    run_id = str(uuid.uuid4())
    started_at_ms = int(time.time() * 1000)

    db = sqlite3.connect(args.db_path)
    configure_connection(db)

    try:
        create_tables_if_needed(db)
        decisions = run_strategy_policy(db, args, config, qlib_class)
        completed_at_ms = int(time.time() * 1000)
        model_run_id_for_run = ""

        db.execute("BEGIN IMMEDIATE;")
        upsert_promotion_profile(db, config, qlib_class, completed_at_ms)
        db.execute(
            """
            INSERT INTO qlib_strategy_runs
            (run_id, strategy_id, qlib_class, config_hash, model_id, model_run_id,
             interval, universe_hash, started_at_ms, completed_at_ms, status, error)
            VALUES (?, ?, ?, ?, ?, '', ?, ?, ?, NULL, 'running', NULL)
            """,
            (
                run_id,
                args.strategy_id,
                qlib_class,
                config_hash,
                args.model_id,
                args.interval,
                args.universe_hash,
                started_at_ms,
            ),
        )

        for decision in decisions:
            model_run_id_for_run = decision.get("model_run_id", "") or model_run_id_for_run
            db.execute(
                """
                INSERT OR REPLACE INTO qlib_strategy_targets
                (strategy_id, run_id, symbol, interval, target_weight, generated_at_ms)
                VALUES (?, ?, ?, ?, ?, ?)
                """,
                (
                    args.strategy_id,
                    run_id,
                    decision["symbol"],
                    args.interval,
                    decision["target_weight"],
                    completed_at_ms,
                ),
            )
            db.execute(
                """
                INSERT OR REPLACE INTO qlib_strategy_decisions
                (strategy_id, run_id, model_id, model_run_id, symbol, interval,
                 asof_open_time_ms, generated_at_ms, action, direction, target_weight,
                 score, score_percentile, confidence, reason)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    args.strategy_id,
                    run_id,
                    args.model_id,
                    decision.get("model_run_id", ""),
                    decision["symbol"],
                    args.interval,
                    decision["asof_open_time_ms"],
                    completed_at_ms,
                    decision["action"],
                    decision["direction"],
                    decision["target_weight"],
                    decision.get("score"),
                    decision.get("score_percentile"),
                    decision["confidence"],
                    decision["reason"],
                ),
            )

        db.execute(
            """
            UPDATE qlib_strategy_runs
            SET model_run_id = ?, completed_at_ms = ?, status = 'succeeded'
            WHERE run_id = ?
            """,
            (model_run_id_for_run, completed_at_ms, run_id),
        )
        db.commit()
        return 0
    except Exception as exc:
        db.rollback()
        try:
            db.execute(
                """
                INSERT OR REPLACE INTO qlib_strategy_runs
                (run_id, strategy_id, qlib_class, config_hash, model_id, model_run_id,
                 interval, universe_hash, started_at_ms, completed_at_ms, status, error)
                VALUES (?, ?, ?, ?, ?, '', ?, ?, ?, ?, 'failed', ?)
                """,
                (
                    run_id,
                    args.strategy_id,
                    qlib_class,
                    config_hash,
                    args.model_id,
                    args.interval,
                    args.universe_hash,
                    started_at_ms,
                    int(time.time() * 1000),
                    str(exc),
                ),
            )
            db.commit()
        except Exception:
            db.rollback()
        print(f"Error running strategy: {exc}", file=sys.stderr)
        return 1
    finally:
        db.close()


if __name__ == "__main__":
    sys.exit(main(parse_args()))
