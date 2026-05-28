import argparse
import os
import sqlite3
import sys
import tempfile
from unittest import mock

import pytest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "../../tools/qlib_bridge")))
import run_strategy

@pytest.fixture
def temp_db():
    fd, path = tempfile.mkstemp()
    os.close(fd)
    db = sqlite3.connect(path)
    db.execute("""
    CREATE TABLE qlib_predictions (
        model_id TEXT,
        run_id TEXT,
        symbol TEXT,
        interval TEXT,
        asof_open_time_ms INTEGER,
        generated_at_ms INTEGER,
        horizon_bars INTEGER,
        score REAL,
        rank INTEGER,
        score_percentile REAL
    )
    """)
    db.commit()
    yield path, db
    db.close()
    os.unlink(path)

def test_run_strategy_allowlist():
    args = argparse.Namespace(
        qlib_class="InvalidClass",
        config_json="{}",
        db_path="",
        strategy_id="",
        model_id="",
        interval="",
        universe_hash=""
    )
    assert run_strategy.main(args) == 1

def test_run_strategy_topk(temp_db):
    db_path, db = temp_db
    
    for i in range(10):
        db.execute("""
            INSERT INTO qlib_predictions (model_id, run_id, symbol, interval, asof_open_time_ms, generated_at_ms, score, score_percentile)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        """, ("model_1", "mrun1", f"SYM{i}", "1h", 1000, 2000, i * 0.1, i * 10.0))
    db.commit()
    
    args = argparse.Namespace(
        qlib_class="qlib.contrib.strategy.TopkDropoutStrategy",
        config_json='{"topk": 3}',
        db_path=db_path,
        strategy_id="strat_1",
        model_id="model_1",
        interval="1h",
        universe_hash="abc"
    )
    
    assert run_strategy.main(args) == 0
    
    cursor = db.execute("SELECT symbol, score FROM qlib_strategy_decisions ORDER BY score DESC")
    rows = cursor.fetchall()
    assert len(rows) == 3
    assert rows[0][0] == "SYM9"
    assert rows[1][0] == "SYM8"
    assert rows[2][0] == "SYM7"
    
    cursor = db.execute("SELECT status FROM qlib_strategy_runs")
    rows = cursor.fetchall()
    assert len(rows) == 1
    assert rows[0][0] == "succeeded"

    assert db.execute("PRAGMA user_version").fetchone()[0] == run_strategy.SCHEMA_VERSION
    assert db.execute("SELECT COUNT(*) FROM qlib_strategy_targets").fetchone()[0] == 3

def insert_prediction_batch(db, generated_at_ms, scores):
    for symbol, score, percentile in scores:
        db.execute("""
            INSERT INTO qlib_predictions (model_id, run_id, symbol, interval, asof_open_time_ms, generated_at_ms, score, score_percentile)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        """, ("model_1", f"mrun{generated_at_ms}", symbol, "1h", generated_at_ms - 1000, generated_at_ms, score, percentile))
    db.commit()

def run_adapter(db_path, qlib_class, config_json, strategy_id="strat_1"):
    args = argparse.Namespace(
        qlib_class=qlib_class,
        config_json=config_json,
        db_path=db_path,
        strategy_id=strategy_id,
        model_id="model_1",
        interval="1h",
        universe_hash="abc"
    )
    assert run_strategy.main(args) == 0

def test_topk_dropout_respects_n_drop(temp_db):
    db_path, db = temp_db
    insert_prediction_batch(db, 2000, [(f"SYM{i}", float(i), i * 10.0) for i in range(6)])

    run_adapter(db_path, "TopkDropoutStrategy", '{"topk": 3, "n_drop": 3}')
    first = {row[0] for row in db.execute("SELECT symbol FROM qlib_strategy_targets")}
    assert first == {"SYM5", "SYM4", "SYM3"}

    insert_prediction_batch(
        db,
        3000,
        [
            ("SYM0", 100.0, 100.0),
            ("SYM1", 90.0, 90.0),
            ("SYM2", 80.0, 80.0),
            ("SYM3", 70.0, 70.0),
            ("SYM4", 60.0, 60.0),
            ("SYM5", 50.0, 50.0),
        ],
    )
    run_adapter(db_path, "TopkDropoutStrategy", '{"topk": 3, "n_drop": 1}')

    latest_run = db.execute(
        "SELECT run_id FROM qlib_strategy_runs WHERE strategy_id = 'strat_1' ORDER BY completed_at_ms DESC LIMIT 1"
    ).fetchone()[0]
    latest = {row[0] for row in db.execute("SELECT symbol FROM qlib_strategy_targets WHERE run_id = ?", (latest_run,))}
    assert latest == {"SYM0", "SYM3", "SYM4"}

def test_soft_topk_limits_turnover(temp_db):
    db_path, db = temp_db
    insert_prediction_batch(db, 2000, [("SYM3", 4.0, 1.0), ("SYM2", 3.0, 0.75), ("SYM1", 2.0, 0.5), ("SYM0", 1.0, 0.25)])
    run_adapter(db_path, "SoftTopkStrategy", '{"topk": 2}')

    insert_prediction_batch(db, 3000, [("SYM0", 4.0, 1.0), ("SYM1", 3.0, 0.75), ("SYM2", 2.0, 0.5), ("SYM3", 1.0, 0.25)])
    run_adapter(db_path, "SoftTopkStrategy", '{"topk": 2, "trade_impact_limit": 0.25}')

    latest_run = db.execute(
        "SELECT run_id FROM qlib_strategy_runs WHERE strategy_id = 'strat_1' ORDER BY completed_at_ms DESC LIMIT 1"
    ).fetchone()[0]
    weights = dict(db.execute(
        "SELECT symbol, target_weight FROM qlib_strategy_targets WHERE run_id = ?",
        (latest_run,),
    ).fetchall())
    assert weights["SYM0"] == pytest.approx(0.5)
    assert weights["SYM2"] == pytest.approx(0.25)
    assert weights["SYM3"] == pytest.approx(0.25)
    assert "SYM1" not in weights

def test_promotion_profile_is_recorded(temp_db):
    db_path, db = temp_db
    insert_prediction_batch(db, 2000, [("SYM1", 1.0, 100.0)])

    run_adapter(
        db_path,
        "TopkDropoutStrategy",
        '{"topk": 1, "promotion_profile": "alpha_topk_dropout_default", "promotion_profile_config": {"min_shadow_signals": 200}}',
    )

    row = db.execute(
        "SELECT qlib_class, profile_json FROM qlib_promotion_profiles WHERE profile_name = ?",
        ("alpha_topk_dropout_default",),
    ).fetchone()
    assert row[0] == "qlib.contrib.strategy.TopkDropoutStrategy"
    assert '"min_shadow_signals": 200' in row[1]
    columns = {item[1] for item in db.execute("PRAGMA table_info(qlib_adapter_runtime_state)").fetchall()}
    assert "promotion_profile" in columns
    assert "last_decision_at_ms" in columns


def test_topk_dropout_handles_large_previous_positions_edge_case(temp_db):
    db_path, db = temp_db
    run_strategy.configure_connection(db)
    run_strategy.create_tables_if_needed(db)

    db.execute(
        """
        INSERT INTO qlib_strategy_runs
        (run_id, strategy_id, qlib_class, config_hash, model_id, model_run_id,
         interval, universe_hash, started_at_ms, completed_at_ms, status, error)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'succeeded', NULL)
        """,
        (
            "prev_run",
            "strat_edge",
            "qlib.contrib.strategy.TopkDropoutStrategy",
            "h",
            "model_1",
            "",
            "1h",
            "abc",
            1000,
            2000,
        ),
    )
    for symbol in ("OLD1", "OLD2", "OLD3", "OLD4", "OLD5"):
        db.execute(
            """
            INSERT INTO qlib_strategy_targets
            (strategy_id, run_id, symbol, interval, target_weight, generated_at_ms)
            VALUES (?, ?, ?, ?, ?, ?)
            """,
            ("strat_edge", "prev_run", symbol, "1h", 0.2, 2000),
        )
    db.commit()

    insert_prediction_batch(
        db,
        5000,
        [
            ("NEW1", 100.0, 100.0),
            ("NEW2", 90.0, 90.0),
            ("OLD1", 80.0, 80.0),
            ("OLD2", 70.0, 70.0),
            ("OLD3", 60.0, 60.0),
            ("OLD4", 50.0, 50.0),
            ("OLD5", 40.0, 40.0),
        ],
    )
    run_adapter(db_path, "TopkDropoutStrategy", '{"topk": 2, "n_drop": 1}', strategy_id="strat_edge")

    latest_run = db.execute(
        "SELECT run_id FROM qlib_strategy_runs WHERE strategy_id = 'strat_edge' ORDER BY completed_at_ms DESC LIMIT 1"
    ).fetchone()[0]
    latest = {row[0] for row in db.execute("SELECT symbol FROM qlib_strategy_targets WHERE run_id = ?", (latest_run,))}
    assert latest == {"NEW1", "OLD1"}


def test_soft_topk_first_fill_normalizes_weight_sum(temp_db):
    db_path, db = temp_db
    run_strategy.configure_connection(db)
    run_strategy.create_tables_if_needed(db)

    db.execute(
        """
        INSERT INTO qlib_strategy_runs
        (run_id, strategy_id, qlib_class, config_hash, model_id, model_run_id,
         interval, universe_hash, started_at_ms, completed_at_ms, status, error)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'succeeded', NULL)
        """,
        (
            "prev_soft",
            "strat_soft_edge",
            "qlib.contrib.strategy.SoftTopkStrategy",
            "h",
            "model_1",
            "",
            "1h",
            "abc",
            1000,
            2000,
        ),
    )
    # Intentionally non-normalized legacy state to verify hardening.
    for symbol, weight in (("OLD1", 0.3), ("OLD2", 0.2), ("OLD3", 0.1)):
        db.execute(
            """
            INSERT INTO qlib_strategy_targets
            (strategy_id, run_id, symbol, interval, target_weight, generated_at_ms)
            VALUES (?, ?, ?, ?, ?, ?)
            """,
            ("strat_soft_edge", "prev_soft", symbol, "1h", weight, 2000),
        )
    db.commit()

    insert_prediction_batch(
        db,
        6000,
        [
            ("NEW1", 100.0, 100.0),
            ("OLD1", 90.0, 90.0),
            ("OLD2", 80.0, 80.0),
            ("OLD3", 70.0, 70.0),
        ],
    )
    run_adapter(db_path, "SoftTopkStrategy", '{"topk": 2, "buy_method": "first_fill", "trade_impact_limit": 0.2}', strategy_id="strat_soft_edge")

    latest_run = db.execute(
        "SELECT run_id FROM qlib_strategy_runs WHERE strategy_id = 'strat_soft_edge' ORDER BY completed_at_ms DESC LIMIT 1"
    ).fetchone()[0]
    weights = dict(
        db.execute(
            "SELECT symbol, target_weight FROM qlib_strategy_targets WHERE run_id = ?",
            (latest_run,),
        ).fetchall()
    )
    assert sum(weights.values()) == pytest.approx(1.0)
    assert "NEW1" in weights


def test_file_order_strategy_validates_order_shape(temp_db):
    db_path, db = temp_db
    args = argparse.Namespace(
        qlib_class="FileOrderStrategy",
        config_json='{"orders":[{"symbol":"BTCUSDT","target_weight":0.5}]}',
        db_path=db_path,
        strategy_id="file_order_bad",
        model_id="model_1",
        interval="1h",
        universe_hash="abc",
    )
    assert run_strategy.main(args) == 1
    row = db.execute(
        "SELECT status, error FROM qlib_strategy_runs WHERE strategy_id = 'file_order_bad' ORDER BY started_at_ms DESC LIMIT 1"
    ).fetchone()
    assert row is not None
    assert row[0] == "failed"
    assert "asof_open_time_ms" in row[1]


def test_file_order_strategy_sorts_none_score_last(temp_db):
    db_path, db = temp_db
    args = argparse.Namespace(
        qlib_class="FileOrderStrategy",
        config_json=(
            '{"orders":['
            '{"symbol":"AAA","asof_open_time_ms":1000,"target_weight":0.4,"score":null,"confidence":0.2},'
            '{"symbol":"BBB","asof_open_time_ms":1000,"target_weight":0.6,"score":0.0,"confidence":0.9}'
            "]}"),
        db_path=db_path,
        strategy_id="file_order_sort",
        model_id="model_1",
        interval="1h",
        universe_hash="abc",
    )
    assert run_strategy.main(args) == 0
    latest_run = db.execute(
        "SELECT run_id FROM qlib_strategy_runs WHERE strategy_id = 'file_order_sort' ORDER BY started_at_ms DESC LIMIT 1"
    ).fetchone()[0]
    symbols = [
        row[0]
        for row in db.execute(
            "SELECT symbol FROM qlib_strategy_decisions WHERE run_id = ? ORDER BY rowid ASC",
            (latest_run,),
        ).fetchall()
    ]
    assert symbols == ["BBB", "AAA"]
