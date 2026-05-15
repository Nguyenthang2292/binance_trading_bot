#include "strategy/istrategy.h"
#include "strategy/strategy_config.h"

#include <nlohmann/json.hpp>

namespace {

class TemplateStrategy final : public strategy::IStrategy {
public:
    explicit TemplateStrategy(strategy::StrategyConfig cfg) : m_cfg(std::move(cfg)) {}

    const strategy::StrategyConfig& config() const override { return m_cfg; }

    strategy::Signal evaluate(
        std::string_view,
        std::string_view,
        const std::vector<Kline>&) const override {
        return {};
    }

private:
    strategy::StrategyConfig m_cfg;
};

strategy::StrategyConfig parseConfig(const nlohmann::json& j) {
    strategy::StrategyConfig cfg;
    cfg.name = j.value("name", "template_strategy");
    cfg.type = j.value("type", "template_strategy");
    cfg.intervals = j.value("intervals", std::vector<std::string>{"15m"});
    cfg.scanInterval = std::chrono::seconds(j.value("scan_interval_seconds", 3600));
    cfg.maxHoldDuration = std::chrono::seconds(j.value("max_hold_duration_seconds", 86400));
    cfg.riskPct = j.value("risk_pct", 0.01);
    cfg.slMultiplier = j.value("sl_multiplier", 1.5);
    cfg.tpMultiplier = j.value("tp_multiplier", 3.0);
    cfg.minNotional = j.value("min_notional", 1.0);
    cfg.atrPeriod = j.value("atr_period", 14);
    cfg.minConfidence = j.value("min_confidence", 0.0);
    return cfg;
}

} // namespace

extern "C" {

__declspec(dllexport) strategy::IStrategy* createStrategy(const char* config_json) {
    try {
        const auto j = nlohmann::json::parse(config_json == nullptr ? "{}" : config_json);
        auto cfg = parseConfig(j);
        return new TemplateStrategy(std::move(cfg));
    } catch (...) {
        return nullptr;
    }
}

__declspec(dllexport) void destroyStrategy(strategy::IStrategy* strategy) {
    delete strategy;
}

__declspec(dllexport) const char* strategyType() {
    return "template_strategy";
}

__declspec(dllexport) const char* pluginVersion() {
    return "1.0.0";
}

} // extern "C"

