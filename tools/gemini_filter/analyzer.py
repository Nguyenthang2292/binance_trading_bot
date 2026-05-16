from __future__ import annotations

from functools import partial
import logging
import os
import threading
import time
from pathlib import Path
from typing import Any, cast

from google import genai
from google.genai import types

from .cache_store import load_entry, save_entry
from .chart_generator import generate_chart
from .gemini_client import generate_json_score, generate_plain_text, parse_score_text
from .key_manager import GeminiKey
from .metrics_store import MetricsStore
from .model_resolver import resolve_models
from .model_router import RoutedModels, build_model_route
from .quota_manager import QuotaConfig, QuotaManager

LOGGER = logging.getLogger("gemini_filter.analyzer")


def _build_sentiment_prompt(symbol: str, base_asset: str, direction: str) -> str:
    return (
        f"Analyze the latest crypto market sentiment for {base_asset} ({symbol}) futures. "
        f"Include macro context from BTC dominance and the Crypto Fear & Greed Index. "
        f"The trading direction under evaluation is {direction}. "
        f"Score favorability for this direction from 0.0 to 1.0."
    )


def _build_vision_prompt(symbol: str, direction: str, primary_tf: str, extra_tfs: list[str]) -> str:
    extra_text = ", ".join(extra_tfs) if extra_tfs else "none"
    return (
        f"You are reviewing candlestick charts for {symbol}. "
        f"Extra context timeframes: {extra_text}. Primary timeframe: {primary_tf}. "
        f"A strategy produced a {direction} signal on {primary_tf}. "
        f"Assess trend, momentum, and support/resistance alignment. "
        f"Score the quality of this {direction} setup from 0.0 to 1.0."
    )


def _generate_json_score_with_rotation(
    client: genai.Client,
    _key: GeminiKey,
    *,
    model: str,
    contents: Any,
    use_google_search: bool,
) -> dict[str, Any]:
    return generate_json_score(
        client=client,
        model=model,
        contents=contents,
        use_google_search=use_google_search,
    )


def _generate_plain_text_with_rotation(
    client: genai.Client,
    _key: GeminiKey,
    *,
    model: str,
    contents: Any,
    use_google_search: bool,
) -> str:
    return generate_plain_text(
        client=client,
        model=model,
        contents=contents,
        use_google_search=use_google_search,
    )


def _is_quota_like_error(exc: Exception) -> bool:
    lowered = str(exc).lower()
    return any(
        token in lowered
        for token in ("429", "quota", "resource_exhausted", "rate limit", "too many requests")
    )


def _run_json_score_with_routes(
    *,
    key_manager: Any,
    quota_manager: QuotaManager,
    candidates: list[str],
    contents: Any,
    use_google_search: bool,
    component: str,
) -> tuple[dict[str, Any], str]:
    attempts: list[str] = []
    for model in candidates:
        reserve = quota_manager.reserve(model)
        if not reserve.ok:
            LOGGER.warning(
                "quota reserve rejected model=%s component=%s reason=%s retry_after_s=%d",
                model,
                component,
                reserve.reason,
                reserve.retry_after_seconds,
            )
            attempts.append(f"{model}:reserve:{reserve.reason}")
            continue
        LOGGER.info("quota reserve ok model=%s component=%s", model, component)
        try:
            result = key_manager.run_with_rotation(
                partial(
                    _generate_json_score_with_rotation,
                    model=model,
                    contents=contents,
                    use_google_search=use_google_search,
                )
            )
            return result, model
        except Exception as exc:  # noqa: BLE001
            if _is_quota_like_error(exc):
                quota_manager.cooldown(model)
                attempts.append(f"{model}:quota:{exc}")
            else:
                attempts.append(f"{model}:error:{exc}")
            continue
    raise RuntimeError(f"quota_exhausted:{component}:{'; '.join(attempts)}")


def _run_plain_text_with_routes(
    *,
    key_manager: Any,
    quota_manager: QuotaManager,
    candidates: list[str],
    contents: Any,
    use_google_search: bool,
    component: str,
) -> tuple[str, str]:
    attempts: list[str] = []
    for model in candidates:
        reserve = quota_manager.reserve(model)
        if not reserve.ok:
            LOGGER.warning(
                "quota reserve rejected model=%s component=%s reason=%s retry_after_s=%d",
                model,
                component,
                reserve.reason,
                reserve.retry_after_seconds,
            )
            attempts.append(f"{model}:reserve:{reserve.reason}")
            continue
        LOGGER.info("quota reserve ok model=%s component=%s", model, component)
        try:
            text = key_manager.run_with_rotation(
                partial(
                    _generate_plain_text_with_rotation,
                    model=model,
                    contents=contents,
                    use_google_search=use_google_search,
                )
            )
            return text, model
        except Exception as exc:  # noqa: BLE001
            if _is_quota_like_error(exc):
                quota_manager.cooldown(model)
                attempts.append(f"{model}:quota:{exc}")
            else:
                attempts.append(f"{model}:error:{exc}")
            continue
    raise RuntimeError(f"quota_exhausted:{component}:{'; '.join(attempts)}")


def _sentiment_cache_key(data: dict[str, Any], route: RoutedModels) -> str:
    symbol = str(data["symbol"])
    base_asset = symbol.replace("USDT", "").replace("BUSD", "")
    direction = str(data["direction"])
    model_key = ",".join(route.sentiment_candidates)
    search = bool(data.get("sentiment_search_then_score", False))
    return f"v1|asset={base_asset}|direction={direction}|models={model_key}|search_then_score={search}"


def _analyze_sentiment(
    data: dict[str, Any],
    key_manager: Any,
    route: RoutedModels,
    quota_manager: QuotaManager,
    metrics_store: MetricsStore,
) -> dict[str, Any]:
    symbol = str(data["symbol"])
    direction = str(data["direction"])
    base_asset = symbol.replace("USDT", "").replace("BUSD", "")
    prompt = _build_sentiment_prompt(symbol, base_asset, direction)
    eval_id = str(data.get("eval_id", ""))
    runtime_base = Path(str(data.get("runtime_base_dir") or data.get("runtime_dir") or "."))
    ttl_seconds = max(0, int(data.get("sentiment_cache_ttl_seconds", 3600)))
    max_stale_seconds = max(ttl_seconds, int(data.get("sentiment_cache_max_stale_seconds", 21600)))
    cache_key = _sentiment_cache_key(data, route)
    now = int(time.time())

    cached = load_entry(runtime_base / "cache", "sentiment", cache_key)
    stale_cached: dict[str, Any] | None = None
    if cached:
        cached_at = int(cached.get("cached_at", 0))
        age = max(0, now - cached_at)
        if age <= ttl_seconds:
            metrics_store.incr_cache("sentiment", "hit")
            LOGGER.info("sentiment cache hit eval_id=%s symbol=%s age_s=%d", eval_id, symbol, age)
            return {
                "score": float(cached["score"]),
                "analysis": str(cached.get("analysis", "")),
            }
        if age <= max_stale_seconds:
            stale_cached = cached

    metrics_store.incr_cache("sentiment", "miss")
    use_two_step = bool(data.get("sentiment_search_then_score", False))
    LOGGER.info("sentiment analysis start eval_id=%s symbol=%s", eval_id, symbol)
    try:
        if not use_two_step:
            result, used_model = _run_json_score_with_routes(
                key_manager=key_manager,
                quota_manager=quota_manager,
                candidates=route.sentiment_candidates,
                contents=prompt,
                use_google_search=True,
                component="sentiment",
            )
        else:
            evidence, _ = _run_plain_text_with_routes(
                key_manager=key_manager,
                quota_manager=quota_manager,
                candidates=route.sentiment_candidates,
                contents=prompt,
                use_google_search=True,
                component="sentiment_evidence",
            )
            score_prompt = (
                "Given the market evidence below, score favorability for the proposed direction. "
                "Return strict JSON with score and analysis.\n\n"
                f"Direction: {direction}\n"
                f"Evidence:\n{evidence}"
            )
            result, used_model = _run_json_score_with_routes(
                key_manager=key_manager,
                quota_manager=quota_manager,
                candidates=route.sentiment_candidates,
                contents=score_prompt,
                use_google_search=False,
                component="sentiment_score",
            )
    except Exception as exc:  # noqa: BLE001
        if _is_quota_like_error(exc) or "quota_exhausted" in str(exc).lower():
            if stale_cached:
                metrics_store.incr_cache("sentiment", "stale_hit")
                LOGGER.warning("sentiment stale fallback eval_id=%s symbol=%s", eval_id, symbol)
                return {
                    "score": float(stale_cached["score"]),
                    "analysis": str(stale_cached.get("analysis", "")),
                }
            raise RuntimeError(f"quota_exhausted:sentiment:{exc}") from exc
        raise

    save_entry(
        runtime_base / "cache",
        "sentiment",
        cache_key,
        {
            "score": float(result["score"]),
            "analysis": str(result.get("analysis", "")),
            "cached_at": now,
            "model": used_model,
        },
    )
    LOGGER.info("sentiment analysis completed eval_id=%s score=%.4f model=%s", eval_id, float(result["score"]), used_model)
    return result


def _analyze_vision(
    data: dict[str, Any],
    key_manager: Any,
    route: RoutedModels,
    quota_manager: QuotaManager,
) -> dict[str, Any]:
    symbol = str(data["symbol"])
    direction = str(data["direction"])
    primary_tf = str(data["primary_tf"])
    extra_tfs = list(data.get("extra_tfs", []))
    eval_id = str(data["eval_id"])
    runtime_dir = str(data["runtime_dir"])
    klines = data["klines"]

    primary_klines = klines.get(primary_tf, [])
    if not isinstance(primary_klines, list) or not primary_klines:
        raise RuntimeError("primary_ohlcv_missing")

    tfs_to_render: dict[str, list[dict[str, Any]]] = {}
    for tf in extra_tfs + [primary_tf]:
        if tf in klines and isinstance(klines[tf], list) and klines[tf]:
            tfs_to_render[tf] = klines[tf]
    if primary_tf not in tfs_to_render:
        raise RuntimeError("primary_ohlcv_missing")

    chart_path = generate_chart(
        klines_by_tf=tfs_to_render,
        primary_tf=primary_tf,
        symbol=symbol,
        output_dir=runtime_dir,
        eval_id=eval_id,
    )
    prompt = _build_vision_prompt(symbol, direction, primary_tf, extra_tfs)
    LOGGER.info("vision analysis start eval_id=%s symbol=%s primary_tf=%s", eval_id, symbol, primary_tf)
    try:
        image_bytes = Path(chart_path).read_bytes()
        image_part = types.Part.from_bytes(data=image_bytes, mime_type="image/png")
        result, used_model = _run_json_score_with_routes(
            key_manager=key_manager,
            quota_manager=quota_manager,
            candidates=route.vision_candidates,
            contents=[prompt, image_part],
            use_google_search=False,
            component="vision",
        )
        score = float(result["score"])
        if route.vision_pro_escalation_enabled and route.vision_pro_escalation_min_score <= score <= route.vision_pro_escalation_max_score:
            pro_candidates = [model for model in route.vision_candidates if "-pro" in model]
            if pro_candidates and used_model not in pro_candidates:
                LOGGER.info("vision escalation to pro eval_id=%s score=%.4f", eval_id, score)
                escalated, used_model = _run_json_score_with_routes(
                    key_manager=key_manager,
                    quota_manager=quota_manager,
                    candidates=pro_candidates,
                    contents=[prompt, image_part],
                    use_google_search=False,
                    component="vision_escalation",
                )
                result = escalated
        LOGGER.info("vision analysis completed eval_id=%s score=%.4f model=%s", eval_id, float(result["score"]), used_model)
        return result
    except Exception as exc:  # noqa: BLE001
        if _is_quota_like_error(exc) or "quota_exhausted" in str(exc).lower():
            raise RuntimeError(f"quota_exhausted:vision:{exc}") from exc
        raise
    finally:
        try:
            os.remove(chart_path)
        except OSError:
            pass


def _build_quota_manager(data: dict[str, Any], metrics_store: MetricsStore) -> QuotaManager:
    runtime_base = Path(str(data.get("runtime_base_dir") or data.get("runtime_dir") or "."))
    quota_cfg_raw = data.get("quota", {})
    if not isinstance(quota_cfg_raw, dict):
        quota_cfg_raw = {}
    quota_cfg = cast(dict[str, Any], quota_cfg_raw)

    limits_raw = quota_cfg.get("models", {})
    limits: dict[str, tuple[int, int]] = {}
    if isinstance(limits_raw, dict):
        limits_dict = cast(dict[str, Any], limits_raw)
        for model_name, item in limits_dict.items():
            if not isinstance(item, dict):
                continue
            item_dict = cast(dict[str, Any], item)
            try:
                rpm = int(item_dict.get("rpm", 0))
                rpd = int(item_dict.get("rpd", 0))
            except Exception:
                continue
            if rpm > 0 and rpd > 0:
                limits[str(model_name)] = (rpm, rpd)

    config = QuotaConfig(
        enabled=bool(quota_cfg.get("enabled", False)),
        safety_factor=max(0.05, min(1.0, float(quota_cfg.get("safety_factor", 0.7)))),
        cooldown_seconds_on_429=max(1, int(quota_cfg.get("cooldown_seconds_on_429", 300))),
        default_rpm=max(1, int(quota_cfg.get("default_rpm", 8))),
        default_rpd=max(1, int(quota_cfg.get("default_rpd", 250))),
        model_limits=limits,
    )
    return QuotaManager(runtime_base, config, metrics_store=metrics_store)


def _build_metrics_store(data: dict[str, Any]) -> MetricsStore:
    runtime_base = Path(str(data.get("runtime_base_dir") or data.get("runtime_dir") or "."))
    return MetricsStore(runtime_base)


def _log_metrics_summary(eval_id: str, metrics_store: MetricsStore) -> None:
    snapshot = metrics_store.snapshot()
    cache = snapshot.get("cache", {})
    if not isinstance(cache, dict):
        cache = {}
    cache_dict = cast(dict[str, Any], cache)
    sentiment_cache = cache_dict.get("sentiment", {})
    if not isinstance(sentiment_cache, dict):
        sentiment_cache = {}
    sentiment_cache_dict = cast(dict[str, Any], sentiment_cache)
    hit = int(sentiment_cache_dict.get("hit", 0))
    miss = int(sentiment_cache_dict.get("miss", 0))
    stale_hit = int(sentiment_cache_dict.get("stale_hit", 0))
    total = hit + miss
    hit_rate = (hit / total) if total > 0 else 0.0
    LOGGER.info(
        "metrics cache eval_id=%s component=sentiment hit=%d miss=%d stale_hit=%d hit_rate=%.4f",
        eval_id,
        hit,
        miss,
        stale_hit,
        hit_rate,
    )

    quota = snapshot.get("quota", {})
    if not isinstance(quota, dict):
        quota = {}
    quota_dict = cast(dict[str, Any], quota)
    for model, model_data in quota_dict.items():
        if not isinstance(model_data, dict):
            continue
        model_data_dict = cast(dict[str, Any], model_data)
        ok = int(model_data_dict.get("reserve_ok", 0))
        reject_rpm = int(model_data_dict.get("reserve_reject_rpm", 0))
        reject_rpd = int(model_data_dict.get("reserve_reject_rpd", 0))
        reject_cooldown = int(model_data_dict.get("reserve_reject_cooldown", 0))
        cooldown_set = int(model_data_dict.get("cooldown_set", 0))
        rejected = reject_rpm + reject_rpd + reject_cooldown
        total = ok + rejected
        success_rate = (ok / total) if total > 0 else 0.0
        LOGGER.info(
            "metrics quota eval_id=%s model=%s reserve_ok=%d reserve_rejected=%d reject_rpm=%d reject_rpd=%d reject_cooldown=%d cooldown_set=%d reserve_success_rate=%.4f",
            eval_id,
            model,
            ok,
            rejected,
            reject_rpm,
            reject_rpd,
            reject_cooldown,
            cooldown_set,
            success_rate,
        )


def analyze(data: dict[str, Any], key_manager: Any) -> dict[str, Any]:
    started = time.perf_counter()
    eval_id = str(data.get("eval_id", ""))
    resolved = resolve_models(data, key_manager)
    metrics_store = _build_metrics_store(data)
    quota_manager = _build_quota_manager(data, metrics_store)

    data = dict(data)
    data["sentiment_model"] = resolved.sentiment_model
    data["vision_model"] = resolved.vision_model
    route = build_model_route(data, resolved.sentiment_model, resolved.vision_model)
    LOGGER.info(
        "analysis start eval_id=%s symbol=%s direction=%s primary_tf=%s model_resolution=%s sentiment_candidates=%s vision_candidates=%s",
        eval_id,
        data.get("symbol", ""),
        data.get("direction", ""),
        data.get("primary_tf", ""),
        resolved.resolution,
        ",".join(route.sentiment_candidates),
        ",".join(route.vision_candidates),
    )
    sentiment_result: dict[str, Any] = {"score": 0.0, "analysis": ""}
    vision_result: dict[str, Any] = {"score": 0.0, "analysis": ""}
    errors: list[str] = []
    lock = threading.Lock()

    def run_sentiment() -> None:
        nonlocal sentiment_result
        try:
            sentiment_result = _analyze_sentiment(data, key_manager, route, quota_manager, metrics_store)
        except Exception as exc:  # noqa: BLE001
            LOGGER.exception("sentiment analysis failed eval_id=%s", eval_id)
            with lock:
                errors.append(f"sentiment_api_error:{exc}")

    def run_vision() -> None:
        nonlocal vision_result
        try:
            vision_result = _analyze_vision(data, key_manager, route, quota_manager)
        except Exception as exc:  # noqa: BLE001
            LOGGER.exception("vision analysis failed eval_id=%s", eval_id)
            with lock:
                errors.append(f"vision_api_error:{exc}")

    t1 = threading.Thread(target=run_sentiment, daemon=True)
    t2 = threading.Thread(target=run_vision, daemon=True)
    t1.start()
    t2.start()
    t1.join(timeout=120.0)
    t2.join(timeout=120.0)
    if t1.is_alive() or t2.is_alive():
        with lock:
            errors.append("component_timeout:analysis threads did not finish within 120s")

    latency_ms = int((time.perf_counter() - started) * 1000)

    if errors:
        error_text = "; ".join(errors)
        error_code = "component_error"
        if "quota_exhausted" in error_text.lower():
            error_code = "quota_exhausted"
        LOGGER.error("analysis blocked eval_id=%s error=%s latency_ms=%d", eval_id, error_text, latency_ms)
        _log_metrics_summary(eval_id, metrics_store)
        return {
            "eval_id": data.get("eval_id", ""),
            "decision": "Block",
            "confidence": 0.0,
            "sentiment_score": 0.0,
            "vision_score": 0.0,
            "sentiment_analysis": sentiment_result.get("analysis", ""),
            "vision_analysis": vision_result.get("analysis", ""),
            "reason": "Gemini component failure",
            "error_code": error_code,
            "error": error_text,
            "latency_ms": latency_ms,
        }

    sentiment_score = float(sentiment_result["score"])
    vision_score = float(vision_result["score"])
    sentiment_weight = float(data["sentiment_weight"])
    vision_weight = float(data["vision_weight"])
    total_weight = sentiment_weight + vision_weight
    confidence = (
        (sentiment_weight * sentiment_score + vision_weight * vision_score) / total_weight
        if total_weight > 0
        else 0.0
    )
    threshold = float(data["confidence_threshold"])
    decision = "Allow" if confidence >= threshold else "Block"
    LOGGER.info(
        "analysis completed eval_id=%s decision=%s confidence=%.4f sentiment=%.4f vision=%.4f latency_ms=%d",
        eval_id,
        decision,
        confidence,
        sentiment_score,
        vision_score,
        latency_ms,
    )
    _log_metrics_summary(eval_id, metrics_store)
    return {
        "eval_id": data.get("eval_id", ""),
        "decision": decision,
        "confidence": max(0.0, min(1.0, confidence)),
        "sentiment_score": max(0.0, min(1.0, sentiment_score)),
        "vision_score": max(0.0, min(1.0, vision_score)),
        "sentiment_analysis": sentiment_result.get("analysis", ""),
        "vision_analysis": vision_result.get("analysis", ""),
        "reason": "Sentiment and vision evaluated",
        "error_code": None,
        "error": None,
        "latency_ms": latency_ms,
    }


def parse_test_score_text(text: str) -> dict[str, Any]:
    return parse_score_text(text)
