from __future__ import annotations

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

