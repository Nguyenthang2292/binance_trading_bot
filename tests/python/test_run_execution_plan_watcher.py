import os
import sqlite3
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "../../tools/qlib_bridge")))
import run_execution_plan_watcher as watcher
import run_strategy


def _connect(db_path: str) -> sqlite3.Connection:
    conn = sqlite3.connect(db_path, isolation_level=None, timeout=10.0)
    run_strategy.configure_connection(conn)
    return conn


def _insert_pending_request(conn: sqlite3.Connection, request_id: str, quantity: str = "1.0") -> None:
    current_ms = int(time.time() * 1000)
    conn.execute(
        """
        INSERT INTO qlib_execution_requests
        (request_id, symbol, side, quantity, position_side, metadata_json, status, created_at_ms, deadline_ms, error)
        VALUES (?, 'BTCUSDT', 'BUY', ?, 'LONG', '{}', 'pending', ?, ?, NULL)
        """,
        (request_id, quantity, current_ms, current_ms + 60_000),
    )


def test_process_once_creates_plan_and_slices():
    fd, db_path = tempfile.mkstemp()
    os.close(fd)
    try:
        conn = _connect(db_path)
        run_strategy.create_tables_if_needed(conn)
        _insert_pending_request(conn, "r1")
        conn.commit()

        processed = watcher.process_once(conn, slice_count=4, duration_ms=60_000)
        assert processed == 1
        assert conn.execute("SELECT COUNT(*) FROM qlib_execution_plans WHERE request_id = 'r1'").fetchone()[0] == 1
        assert conn.execute("SELECT status FROM qlib_execution_plans WHERE request_id = 'r1'").fetchone()[0] == "running"
        assert conn.execute("SELECT COUNT(*) FROM qlib_execution_slices").fetchone()[0] == 4
        conn.close()
    finally:
        os.unlink(db_path)


def test_process_once_concurrent_workers_no_duplicate_plans():
    fd, db_path = tempfile.mkstemp()
    os.close(fd)
    try:
        setup_conn = _connect(db_path)
        run_strategy.create_tables_if_needed(setup_conn)
        request_ids = [f"r{i}" for i in range(20)]
        for request_id in request_ids:
            _insert_pending_request(setup_conn, request_id)
        setup_conn.commit()
        setup_conn.close()

        barrier = threading.Barrier(2)

        def worker() -> int:
            conn = _connect(db_path)
            try:
                barrier.wait(timeout=5)
                return watcher.process_once(conn, slice_count=4, duration_ms=60_000)
            finally:
                conn.close()

        with ThreadPoolExecutor(max_workers=2) as executor:
            results = [future.result(timeout=30) for future in (executor.submit(worker), executor.submit(worker))]

        verify_conn = _connect(db_path)
        plan_count = verify_conn.execute("SELECT COUNT(*) FROM qlib_execution_plans").fetchone()[0]
        distinct_count = verify_conn.execute("SELECT COUNT(DISTINCT request_id) FROM qlib_execution_plans").fetchone()[0]
        slice_count = verify_conn.execute("SELECT COUNT(*) FROM qlib_execution_slices").fetchone()[0]
        max_dups = verify_conn.execute(
            """
            SELECT COALESCE(MAX(c), 0)
            FROM (
                SELECT COUNT(*) AS c
                FROM qlib_execution_plans
                GROUP BY request_id
            )
            """
        ).fetchone()[0]
        verify_conn.close()

        assert sum(results) == 20
        assert plan_count == 20
        assert distinct_count == 20
        assert slice_count == 80
        assert max_dups == 1
    finally:
        os.unlink(db_path)


def test_create_twap_plan_is_idempotent_with_same_request():
    fd, db_path = tempfile.mkstemp()
    os.close(fd)
    try:
        conn = _connect(db_path)
        run_strategy.create_tables_if_needed(conn)
        _insert_pending_request(conn, "r_same")
        conn.commit()

        request = watcher.fetch_pending_request(conn)
        assert request is not None
        watcher.create_twap_plan(conn, request, default_slice_count=4, default_duration_ms=60_000)
        watcher.create_twap_plan(conn, request, default_slice_count=4, default_duration_ms=60_000)
        conn.commit()

        assert conn.execute("SELECT COUNT(*) FROM qlib_execution_plans WHERE request_id = 'r_same'").fetchone()[0] == 1
        assert conn.execute("SELECT COUNT(*) FROM qlib_execution_slices").fetchone()[0] == 4
        conn.close()
    finally:
        os.unlink(db_path)
