#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import logging
import signal
import sqlite3
import sys
import time
import uuid
from decimal import Decimal, ROUND_DOWN

from run_strategy import SCHEMA_VERSION, configure_connection, create_tables_if_needed

logging.basicConfig(level=logging.INFO, format="[%(asctime)s][%(levelname)s] %(message)s")

STOP = False


def _stop(_signum: int, _frame: object) -> None:
    global STOP
    STOP = True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Execution Plan Watcher Daemon")
    parser.add_argument("--db-path", required=True, help="Path to sqlite DB")
    parser.add_argument("--model-id", required=True, help="Model ID for logs/supervision")
    parser.add_argument("--poll-interval-seconds", type=float, default=0.2)
    parser.add_argument("--slice-count", type=int, default=4)
    parser.add_argument("--plan-duration-ms", type=int, default=60_000)
    parser.add_argument("--once", action="store_true")
    return parser.parse_args()


def now_ms() -> int:
    return int(time.time() * 1000)


def split_quantity(quantity: str, count: int) -> list[str]:
    count = max(count, 1)
    qty = Decimal(quantity)
    if qty <= 0:
        raise ValueError("quantity must be positive")
    quantum = Decimal("0.00000001")
    base = (qty / count).quantize(quantum, rounding=ROUND_DOWN)
    slices = [base for _ in range(count)]
    remainder = qty - sum(slices)
    slices[-1] += remainder
    return [format(item.normalize(), "f") for item in slices if item > 0]


def fetch_pending_requests(db: sqlite3.Connection, limit: int = 25) -> list[sqlite3.Row]:
    db.row_factory = sqlite3.Row
    return list(
        db.execute(
            """
            SELECT r.*
            FROM qlib_execution_requests r
            LEFT JOIN qlib_execution_plans p ON p.request_id = r.request_id
            WHERE r.status = 'pending' AND p.request_id IS NULL
            ORDER BY r.created_at_ms ASC
            LIMIT ?
            """,
            (limit,),
        )
    )


def mark_expired(db: sqlite3.Connection, request_id: str, reason: str) -> None:
    db.execute(
        "UPDATE qlib_execution_requests SET status = 'expired', error = ? WHERE request_id = ? AND status = 'pending'",
        (reason, request_id),
    )


def plan_config_for_request(request: sqlite3.Row, default_slice_count: int, default_duration_ms: int) -> tuple[int, int]:
    slice_count = default_slice_count
    duration_ms = default_duration_ms
    metadata_raw = request["metadata_json"] or "{}"
    try:
        metadata = json.loads(metadata_raw)
    except json.JSONDecodeError:
        metadata = {}
    if isinstance(metadata, dict):
        twap = metadata.get("twap")
        if isinstance(twap, dict):
            slice_count = int(twap.get("slice_count", slice_count))
            duration_ms = int(twap.get("duration_ms", duration_ms))
        slice_count = int(metadata.get("twap_slice_count", metadata.get("slice_count", slice_count)))
        duration_ms = int(metadata.get("twap_duration_ms", metadata.get("duration_ms", duration_ms)))
    return max(slice_count, 1), max(duration_ms, 1)


def create_twap_plan(
    db: sqlite3.Connection,
    request: sqlite3.Row,
    default_slice_count: int,
    default_duration_ms: int,
) -> None:
    current_ms = now_ms()
    if int(request["deadline_ms"]) <= current_ms:
        mark_expired(db, request["request_id"], "deadline expired before watcher plan")
        return

    slice_count, duration_ms = plan_config_for_request(request, default_slice_count, default_duration_ms)
    quantities = split_quantity(request["quantity"], slice_count)
    if not quantities:
        mark_expired(db, request["request_id"], "zero quantity after slice split")
        return

    plan_id = str(uuid.uuid4())
    expires_at_ms = current_ms + max(duration_ms, 1)
    spacing = max(duration_ms // max(len(quantities), 1), 1)

    db.execute(
        """
        INSERT INTO qlib_execution_plans
        (plan_id, request_id, algorithm, status, generated_at_ms, expires_at_ms, total_quantity, slice_count, error)
        VALUES (?, ?, 'TWAP', 'running', ?, ?, ?, ?, NULL)
        """,
        (plan_id, request["request_id"], current_ms, expires_at_ms, request["quantity"], len(quantities)),
    )
    for index, quantity in enumerate(quantities):
        db.execute(
            """
            INSERT INTO qlib_execution_slices
            (slice_id, plan_id, slice_index, due_at_ms, side, quantity, status, revoked_at_ms, revoke_reason)
            VALUES (?, ?, ?, ?, ?, ?, 'pending', NULL, NULL)
            """,
            (
                str(uuid.uuid4()),
                plan_id,
                index,
                current_ms + index * spacing,
                request["side"],
                quantity,
            ),
        )
    db.execute(
        "UPDATE qlib_execution_plans SET status = 'succeeded' WHERE plan_id = ?",
        (plan_id,),
    )


def process_once(db: sqlite3.Connection, slice_count: int, duration_ms: int) -> int:
    processed = 0
    for request in fetch_pending_requests(db):
        try:
            db.execute("BEGIN IMMEDIATE;")
            create_twap_plan(db, request, slice_count, duration_ms)
            db.commit()
            processed += 1
        except Exception as exc:
            db.rollback()
            logging.exception("failed to create execution plan request_id=%s", request["request_id"])
            db.execute("BEGIN IMMEDIATE;")
            db.execute(
                "UPDATE qlib_execution_requests SET status = 'failed', error = ? WHERE request_id = ?",
                (str(exc), request["request_id"]),
            )
            db.commit()
    return processed


def main() -> int:
    args = parse_args()
    if args.slice_count <= 0:
        raise ValueError("--slice-count must be positive")
    if args.plan_duration_ms <= 0:
        raise ValueError("--plan-duration-ms must be positive")

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    logging.info(
        "Starting execution plan watcher model_id=%s db=%s schema=%s",
        args.model_id,
        args.db_path,
        SCHEMA_VERSION,
    )
    db = sqlite3.connect(args.db_path, isolation_level=None)
    configure_connection(db)
    create_tables_if_needed(db)

    try:
        while not STOP:
            processed = process_once(db, args.slice_count, args.plan_duration_ms)
            if args.once:
                return 0
            if processed == 0:
                time.sleep(args.poll_interval_seconds)
    finally:
        db.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        logging.exception("watcher terminated with error: %s", exc)
        sys.exit(1)
