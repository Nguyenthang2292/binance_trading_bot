from __future__ import annotations

import json
from pathlib import Path

from tools.gemini_filter.metrics_store import MetricsStore


def test_metrics_store_increments_cache_and_quota(tmp_path: Path) -> None:
    store = MetricsStore(tmp_path)
    store.incr_cache("sentiment", "hit")
    store.incr_cache("sentiment", "miss", delta=2)
    store.incr_quota("gemini-2.5-flash", "reserve_ok", delta=3)

    snapshot = store.snapshot()
    assert snapshot["cache"]["sentiment"]["hit"] == 1
    assert snapshot["cache"]["sentiment"]["miss"] == 2
    assert snapshot["quota"]["gemini-2.5-flash"]["reserve_ok"] == 3


def test_metrics_store_records_latency_and_decision_bucket(tmp_path: Path) -> None:
    store = MetricsStore(tmp_path)
    store.record_analyzer_latency("gemini-2.5-flash-lite", "sentiment", 235)
    store.record_decision("Block", "quota_exhausted")

    buckets = sorted((tmp_path / "cache" / "metrics" / "buckets").glob("*.json"))
    assert buckets
    payload = json.loads(buckets[-1].read_text(encoding="utf-8"))
    model_data = payload["analyzer"]["gemini-2.5-flash-lite"]
    stats = model_data["latency_stats"]
    assert model_data["sentiment_calls"] == 1
    assert stats["count"] == 1
    assert stats["sum_ms"] == 235
    assert payload["decisions"]["block"] == 1
    assert payload["decisions"]["quota_exhausted"] == 1
