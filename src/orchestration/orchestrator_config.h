#pragma once

#include "orchestration/batch_scheduler_thread.h"
#include "orchestration/candle_scheduler_thread.h"
#include "orchestration/process_manager.h"
#include "orchestration/promotion_checker.h"
#include "orchestration/qlib_state_store.h"
#include "orchestration/shadow_metrics_recorder.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace orchestration {

struct OrchestratorConfig {
    bool enabled{false};
    BatchSchedulerConfig batch;
    CandleSchedulerConfig candle;
    ProcessManagerConfig process;
    PromotionConfig promotion;
    QlibStateStoreConfig stateStore;
    ShadowMetricsConfig shadowMetrics;
};

inline std::vector<std::string> parseSymbols(const nlohmann::json& j) {
    std::vector<std::string> out;
    if (!j.is_array()) {
        return out;
    }
    for (const auto& item : j) {
        if (item.is_string()) {
            out.push_back(item.get<std::string>());
        }
    }
    return out;
}

inline OrchestratorConfig parseOrchestratorConfig(const nlohmann::json& root) {
    OrchestratorConfig cfg;
    const auto orch = root.value("qlib_orchestration", nlohmann::json::object());
    cfg.enabled = orch.value("enabled", false);
    if (!cfg.enabled) {
        return cfg;
    }

    const std::string pythonExe = orch.value("python_exe", std::string("python"));
    const std::string scriptsDir = orch.value("scripts_dir", std::string("tools/qlib_bridge"));
    const std::string dataDir = orch.value("data_dir", std::string("data/qlib"));
    const std::string modelDir = orch.value("model_dir", std::string("models/qlib"));
    const std::string dbPath = orch.value("db_path", std::string("data/qlib_predictions.db"));
    const std::string modelId = orch.value("model_id", std::string("lightgbm_1h_v1"));
    const std::string interval = orch.value("interval", std::string("1h"));
    const int horizonBars = std::max(1, orch.value("horizon_bars", 4));
    const auto symbols = parseSymbols(orch.value("symbols", nlohmann::json::array()));

    const auto batchSchedule = orch.value("batch_schedule", nlohmann::json::object());
    const auto retry = orch.value("retry", nlohmann::json::object());
    const auto promotion = orch.value("promotion", nlohmann::json::object());
    const auto state = orch.value("state", nlohmann::json::object());
    const auto canary = orch.value("canary", nlohmann::json::object());
    const auto costModel = orch.value("cost_model", nlohmann::json::object());
    const auto candleSchedule = orch.value("candle_schedule", nlohmann::json::object());

    cfg.process.maxAttempts = retry.value("max_attempts", cfg.process.maxAttempts);
    cfg.process.backoffBaseSeconds = retry.value("backoff_base_seconds", cfg.process.backoffBaseSeconds);
    cfg.process.timeoutSeconds = retry.value("timeout_seconds", cfg.process.timeoutSeconds);
    cfg.process.logDir = orch.value("log_dir", cfg.process.logDir);

    cfg.batch.pythonExe = pythonExe;
    cfg.batch.scriptsDir = scriptsDir;
    cfg.batch.dataDir = dataDir;
    cfg.batch.modelDir = modelDir;
    cfg.batch.dbPath = dbPath;
    cfg.batch.modelId = modelId;
    cfg.batch.interval = interval;
    cfg.batch.symbols = symbols;
    cfg.batch.horizonBars = horizonBars;
    cfg.batch.logDir = cfg.process.logDir;
    cfg.batch.batchHour = batchSchedule.value("hour", cfg.batch.batchHour);
    cfg.batch.batchMinute = batchSchedule.value("minute", cfg.batch.batchMinute);
    if (batchSchedule.contains("weekdays") && batchSchedule.at("weekdays").is_array()) {
        cfg.batch.batchWeekdays.clear();
        for (const auto& d : batchSchedule.at("weekdays")) {
            if (d.is_number_integer()) {
                cfg.batch.batchWeekdays.push_back(d.get<int>());
            }
        }
    }

    cfg.candle.pythonExe = pythonExe;
    cfg.candle.scriptsDir = scriptsDir;
    cfg.candle.dataDir = dataDir;
    cfg.candle.modelDir = modelDir;
    cfg.candle.dbPath = dbPath;
    cfg.candle.modelId = modelId;
    cfg.candle.interval = interval;
    cfg.candle.postCandleDelaySeconds =
        candleSchedule.value("finalize_delay_seconds", cfg.candle.postCandleDelaySeconds);
    cfg.candle.readyDir = orch.value("ready_dir", cfg.candle.readyDir);

    cfg.promotion.minCandles = promotion.value("min_candles", cfg.promotion.minCandles);
    cfg.promotion.minSharpe = promotion.value("min_sharpe", cfg.promotion.minSharpe);
    cfg.promotion.minHitRate = promotion.value("min_hit_rate", cfg.promotion.minHitRate);
    cfg.promotion.minMeanNetReturnBps =
        promotion.value("min_mean_net_return_bps", cfg.promotion.minMeanNetReturnBps);
    cfg.promotion.lookbackCandles = promotion.value("lookback_candles", cfg.promotion.lookbackCandles);
    cfg.promotion.horizonBars = horizonBars;

    cfg.stateStore.dbPath = dbPath;
    cfg.stateStore.modelId = modelId;
    cfg.stateStore.interval = interval;
    cfg.stateStore.reloadInterval =
        std::chrono::seconds(std::max(1, state.value("reload_seconds", 5)));
    cfg.stateStore.canaryRiskMultiplier = canary.value("risk_multiplier", 0.25);

    cfg.shadowMetrics.dbPath = dbPath;
    cfg.shadowMetrics.modelId = modelId;
    cfg.shadowMetrics.interval = interval;
    cfg.shadowMetrics.horizonBars = horizonBars;
    cfg.shadowMetrics.costModel.estimatedRoundTripFeeBps =
        costModel.value("estimated_round_trip_fee_bps", cfg.shadowMetrics.costModel.estimatedRoundTripFeeBps);
    cfg.shadowMetrics.costModel.estimatedSlippageBps =
        costModel.value("estimated_slippage_bps", cfg.shadowMetrics.costModel.estimatedSlippageBps);
    cfg.shadowMetrics.costModel.estimatedFundingBpsPerDay =
        costModel.value("estimated_funding_bps_per_day", cfg.shadowMetrics.costModel.estimatedFundingBpsPerDay);
    auto sanitizeCost = [](double value) {
        return std::isfinite(value) && value >= 0.0 ? value : 0.0;
    };
    cfg.shadowMetrics.costModel.estimatedRoundTripFeeBps =
        sanitizeCost(cfg.shadowMetrics.costModel.estimatedRoundTripFeeBps);
    cfg.shadowMetrics.costModel.estimatedSlippageBps =
        sanitizeCost(cfg.shadowMetrics.costModel.estimatedSlippageBps);
    cfg.shadowMetrics.costModel.estimatedFundingBpsPerDay =
        sanitizeCost(cfg.shadowMetrics.costModel.estimatedFundingBpsPerDay);

    return cfg;
}

} // namespace orchestration
