#include "account/account_service.h"
#include "catalog/catalog_reporter.h"
#include "catalog/plugin_loader.h"
#include "catalog/strategy_catalog.h"
#include "context.h"
#include "engine/exposure_controller.h"
#include "engine/gemini_filter.h"
#include "engine/signal_engine.h"
#include "logger.h"
#include "orders/orders.h"
#include "orders/rest_client_adapter.h"
#include "risk/equity_curve.h"
#include "risk/risk_controller.h"
#include "risk/risk_db.h"
#include "risk/risk_metrics.h"
#include "scanner/market_scanner.h"
#include "strategy/strategy_registry.h"
#include "ws/user_data_stream.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_future.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

std::atomic<bool> g_running{true};

void terminateHandler() noexcept {
    try {
        const auto ep = std::current_exception();
        if (ep) {
            std::rethrow_exception(ep);
        }
        Logger::instance().log(LogLevel::Error, "Unhandled std::terminate without active exception");
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::Error, std::string("Unhandled std::terminate: ") + e.what());
    } catch (...) {
        Logger::instance().log(LogLevel::Error, "Unhandled std::terminate with unknown exception");
    }
    std::abort();
}

void signalHandler(int signum) {
    Logger::instance().log(LogLevel::Info, "Received signal " + std::to_string(signum) + ", shutting down...");
    g_running = false;
}

template <typename T>
T syncAwait(boost::asio::io_context& ioc, boost::asio::awaitable<T> task) {
    auto future = boost::asio::co_spawn(ioc, std::move(task), boost::asio::use_future);
    return future.get();
}

std::vector<nlohmann::json> toStrategyConfigs(const nlohmann::json& cfg) {
    std::vector<nlohmann::json> out;
    const auto strategies = cfg.value("strategies", nlohmann::json::array());
    out.reserve(strategies.size());
    for (const auto& item : strategies) {
        out.push_back(item);
    }
    return out;
}

std::string trimCopy(std::string value) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string quoteString(std::string_view value) {
    std::ostringstream out;
    out << std::quoted(std::string(value));
    return out.str();
}

std::optional<Socks5ProxyConfig> parseSocks5Proxy(std::string_view raw, std::string& error) {
    std::string value = trimCopy(std::string(raw));
    if (value.empty()) {
        return std::nullopt;
    }

    std::string authority = value;
    if (const auto schemePos = value.find("://"); schemePos != std::string::npos) {
        const std::string scheme = lowerCopy(value.substr(0, schemePos));
        if (scheme != "socks5" && scheme != "socks5h") {
            error = "unsupported proxy scheme: " + scheme + " (expected socks5:// or socks5h://)";
            return std::nullopt;
        }
        authority = value.substr(schemePos + 3);
    }

    if (const auto slashPos = authority.find_first_of("/?#"); slashPos != std::string::npos) {
        authority = authority.substr(0, slashPos);
    }

    if (authority.empty()) {
        error = "proxy address is empty";
        return std::nullopt;
    }
    if (authority.find('@') != std::string::npos) {
        error = "proxy authentication is not supported";
        return std::nullopt;
    }

    std::string host;
    std::string portText;
    if (authority.front() == '[') {
        const auto closePos = authority.find(']');
        if (closePos == std::string::npos) {
            error = "invalid IPv6 proxy format";
            return std::nullopt;
        }
        host = authority.substr(1, closePos - 1);
        if (closePos + 1 >= authority.size() || authority[closePos + 1] != ':') {
            error = "proxy port is required";
            return std::nullopt;
        }
        portText = authority.substr(closePos + 2);
    } else {
        const auto colonPos = authority.rfind(':');
        if (colonPos == std::string::npos) {
            error = "proxy port is required";
            return std::nullopt;
        }
        host = authority.substr(0, colonPos);
        portText = authority.substr(colonPos + 1);
    }

    host = trimCopy(host);
    portText = trimCopy(portText);
    if (host.empty() || portText.empty()) {
        error = "proxy host/port is invalid";
        return std::nullopt;
    }

    int parsedPort = 0;
    const auto* begin = portText.data();
    const auto* end = begin + portText.size();
    const auto parseResult = std::from_chars(begin, end, parsedPort);
    if (parseResult.ec != std::errc{} || parseResult.ptr != end || parsedPort <= 0 || parsedPort > 65535) {
        error = "proxy port is out of range: " + portText;
        return std::nullopt;
    }

    return Socks5ProxyConfig{.host = host, .port = static_cast<std::uint16_t>(parsedPort)};
}

engine::GeminiFilterMode parseGeminiFilterMode(std::string modeRaw) {
    const std::string mode = lowerCopy(trimCopy(std::move(modeRaw)));
    if (mode == "disabled") {
        return engine::GeminiFilterMode::Disabled;
    }
    if (mode == "enforce") {
        return engine::GeminiFilterMode::Enforce;
    }
    if (mode == "shadow") {
        Logger::instance().log(LogLevel::Warning, "gemini_filter.mode=shadow has been removed; forcing enforce");
        return engine::GeminiFilterMode::Enforce;
    }
    Logger::instance().log(
        LogLevel::Warning,
        "unsupported gemini_filter.mode=" + quoteString(mode) + "; forcing enforce");
    return engine::GeminiFilterMode::Enforce;
}

std::vector<std::string> strategyIntervals(const nlohmann::json& strategyCfg) {
    const auto type = strategyCfg.value("type", std::string{});
    auto intervals = strategyCfg.value("intervals", std::vector<std::string>{});
    if (!intervals.empty()) {
        return intervals;
    }
    if (type == "trend_breakout") {
        return {"30m", "1h", "4h"};
    }
    if (type == "gartley_day_crossover") {
        return {"1d", "4h", "1h", "30m"};
    }
    if (type == "golden_crossover") {
        return {"4h", "1h", "30m"};
    }
    return intervals;
}

int minWarmupCandles(const nlohmann::json& strategyCfg) {
    int atrPeriod = strategyCfg.value("atr_period", 14);
    if (atrPeriod <= 0) {
        atrPeriod = 14;
    }
    int minCandles = atrPeriod + 1;

    const auto type = strategyCfg.value("type", std::string{});
    if (type == "trend_breakout") {
        int breakoutPeriod = 20;
        if (strategyCfg.contains("params") && strategyCfg.at("params").is_object()) {
            breakoutPeriod = strategyCfg.at("params").value("breakout_period", 20);
        }
        if (breakoutPeriod <= 0) {
            breakoutPeriod = 20;
        }
        minCandles = std::max(minCandles, breakoutPeriod + 2);
    } else if (type == "gartley_day_crossover") {
        int fastPeriod = 3;
        int slowPeriod = 6;
        int offset = 2;
        if (strategyCfg.contains("params") && strategyCfg.at("params").is_object()) {
            const auto& params = strategyCfg.at("params");
            fastPeriod = params.value("fast_period", 3);
            slowPeriod = params.value("slow_period", 6);
            offset = params.value("offset", 2);
        }
        if (fastPeriod <= 0) {
            fastPeriod = 3;
        }
        if (slowPeriod <= 0) {
            slowPeriod = 6;
        }
        if (offset < 0) {
            offset = 2;
        }
        minCandles = std::max(minCandles, std::max(fastPeriod + 1, 1 + offset + slowPeriod));
    } else if (type == "golden_crossover") {
        int maShort = 50;
        int maLong = 200;
        if (strategyCfg.contains("params") && strategyCfg.at("params").is_object()) {
            const auto& params = strategyCfg.at("params");
            maShort = params.value("ma_short", 50);
            maLong = params.value("ma_long", 200);
        }
        if (maShort <= 0) {
            maShort = 50;
        }
        if (maLong <= maShort) {
            maLong = 200;
        }
        minCandles = std::max(minCandles, maLong);
    }

    return std::max(1, minCandles);
}

void logScannerCoverageWarnings(
    const std::vector<nlohmann::json>& strategiesConfig,
    const std::vector<std::string>& scannerIntervals,
    size_t warmupInitialLimit) {
    const std::unordered_set<std::string> scannerSet(scannerIntervals.begin(), scannerIntervals.end());
    for (const auto& strategyCfg : strategiesConfig) {
        const auto strategyName = strategyCfg.value("name", strategyCfg.value("type", std::string("unknown")));
        const auto intervals = strategyIntervals(strategyCfg);
        const auto requiredCandles = minWarmupCandles(strategyCfg);

        for (const auto& interval : intervals) {
            if (scannerSet.find(interval) == scannerSet.end()) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "strategy interval not scanned strategy=" + quoteString(strategyName) +
                        " interval=" + interval);
            }
            if (warmupInitialLimit < static_cast<size_t>(requiredCandles)) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "strategy warmup insufficient strategy=" + quoteString(strategyName) +
                        " interval=" + interval +
                        " warmup_initial_limit=" + std::to_string(warmupInitialLimit) +
                        " min_candles=" + std::to_string(requiredCandles));
            }
        }
    }
}

struct StrategyRegistryCleanup {
    strategy::StrategyRegistry& registry;

    ~StrategyRegistryCleanup() {
        registry.clear();
    }
};

int main(int argc, char* argv[]) {
    Logger::instance().setLogFile("trading_bot.log");
    Logger::instance().setMinLevel(LogLevel::Info);
    std::set_terminate(terminateHandler);
    Logger::instance().log(LogLevel::Info, "Binance Trading Bot started");

    nlohmann::json config = nlohmann::json::object();
    try {
        std::ifstream in("config.json");
        if (in) {
            in >> config;
        }
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::Warning, std::string("Failed to parse config.json: ") + e.what());
    }

    const auto catalogJson = config.value("catalog", nlohmann::json::object());
    const auto pluginsDir = catalogJson.value("plugins_dir", "plugins");
    const bool enforceSha256Allowlist = catalogJson.value("enforce_sha256_allowlist", false);
    const auto sha256AllowlistFile = catalogJson.value("sha256_allowlist_file", "");
    const auto strategyConfigs = toStrategyConfigs(config);
    strategy::StrategyRegistry registry;
    catalog::PluginLoader pluginLoader(
        {.pluginsDir = pluginsDir,
         .enforceSha256Allowlist = enforceSha256Allowlist,
         .sha256AllowlistFile = sha256AllowlistFile});
    catalog::StrategyCatalog strategyCatalog(
        {.pluginsDir = pluginsDir,
         .enforceSha256Allowlist = enforceSha256Allowlist,
         .sha256AllowlistFile = sha256AllowlistFile},
        registry,
        std::move(pluginLoader));
    StrategyRegistryCleanup registryCleanup{registry};
    const auto catalogSummary = strategyCatalog.initialize(strategyConfigs);

    if (argc > 1 && std::string_view(argv[1]) == "--list-strategies") {
        catalog::CatalogReporter::printList(
            strategyCatalog.listStrategies(),
            catalogSummary.pluginsLoaded,
            pluginsDir);
        return 0;
    }

    catalog::CatalogReporter::logStartupSummary(catalogSummary, strategyCatalog.listStrategies());

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    const char* apiKey = std::getenv("BINANCE_API_KEY");
    const char* secretKey = std::getenv("BINANCE_SECRET_KEY");
    if (!apiKey || !secretKey) {
        Logger::instance().log(
            LogLevel::Error,
            "Please set BINANCE_API_KEY and BINANCE_SECRET_KEY environment variables");
        return 1;
    }

    const auto scannerJson = config.value("scanner", nlohmann::json::object());
    const auto engineJson = config.value("engine", nlohmann::json::object());
    const auto exposureJson = config.value("exposure_control", nlohmann::json::object());
    const auto orderCapJson = config.value("order_cap", nlohmann::json::object());
    const auto geminiJson = config.value("gemini_filter", nlohmann::json::object());
    const auto riskJson = config.value("risk_analytics", nlohmann::json::object());

    engine::ExposureConfig exposureConfig;
    exposureConfig.enabled = exposureJson.value("enabled", false);
    exposureConfig.targetNetBeta = exposureJson.value("target_net_beta", exposureConfig.targetNetBeta);
    exposureConfig.softLimitNetBeta = exposureJson.value("soft_limit_net_beta", exposureConfig.softLimitNetBeta);
    exposureConfig.hardLimitNetBeta = exposureJson.value("hard_limit_net_beta", exposureConfig.hardLimitNetBeta);
    exposureConfig.maxGrossBeta = exposureJson.value("max_gross_beta", exposureConfig.maxGrossBeta);
    exposureConfig.defaultBeta = exposureJson.value("default_beta", exposureConfig.defaultBeta);
    exposureConfig.minNotionalAfterScale =
        exposureJson.value("min_notional_after_scale", exposureConfig.minNotionalAfterScale);
    exposureConfig.betaWindowDays = exposureJson.value("beta_window_days", exposureConfig.betaWindowDays);
    const auto failureMode = exposureJson.value("failure_mode", std::string("closed"));
    if (failureMode == "open") {
        exposureConfig.failureMode = engine::ExposureFailureMode::Open;
    } else {
        exposureConfig.failureMode = engine::ExposureFailureMode::Closed;
    }

    engine::OrderCapConfig orderCapConfig;
    orderCapConfig.enabled = orderCapJson.value("enabled", orderCapConfig.enabled);
    orderCapConfig.maxTotalNotionalPct =
        orderCapJson.value("max_total_notional_pct", orderCapConfig.maxTotalNotionalPct);
    const auto orderCapFailureMode = orderCapJson.value("failure_mode", std::string("closed"));
    if (orderCapFailureMode == "open") {
        orderCapConfig.failureMode = engine::OrderCapFailureMode::Open;
    } else {
        orderCapConfig.failureMode = engine::OrderCapFailureMode::Closed;
    }

    const bool betaDailyEnabled = scannerJson.value("beta_daily_klines_enabled", exposureConfig.enabled);
    const auto betaDailyInterval = scannerJson.value("beta_daily_interval", std::string("1d"));
    const int betaDailyLimit =
        scannerJson.value("beta_daily_limit", std::max(2, exposureConfig.betaWindowDays + 1));
    const auto scannerIntervals = scannerJson.value("intervals", std::vector<std::string>{"30m", "1h", "4h"});
    const size_t scannerBufferSize = static_cast<size_t>(scannerJson.value("kline_buffer_size", 200));
    const size_t warmupInitialLimitRaw = static_cast<size_t>(scannerJson.value("warmup_initial_limit", 99));
    const size_t warmupInitialLimit = std::max<size_t>(1, std::min(warmupInitialLimitRaw, scannerBufferSize));
    const size_t warmupConcurrency = std::max<size_t>(1, static_cast<size_t>(scannerJson.value("warmup_concurrency", 10)));
    const bool backfillEnabled = scannerJson.value("backfill_enabled", true);
    const size_t backfillConcurrency =
        std::max<size_t>(1, static_cast<size_t>(scannerJson.value("backfill_concurrency", 1)));
    logScannerCoverageWarnings(strategyConfigs, scannerIntervals, warmupInitialLimit);

    engine::GeminiFilterConfig geminiConfig;
    geminiConfig.enabled = geminiJson.value("enabled", geminiConfig.enabled);
    geminiConfig.mode = parseGeminiFilterMode(geminiJson.value("mode", std::string("enforce")));
    geminiConfig.pythonPath = geminiJson.value("python_path", geminiConfig.pythonPath);
    geminiConfig.moduleName = geminiJson.value("module_name", geminiConfig.moduleName);
    geminiConfig.workingDirectory = geminiJson.value("working_directory", geminiConfig.workingDirectory);
    geminiConfig.runtimeDir = geminiJson.value("runtime_dir", geminiConfig.runtimeDir);
    geminiConfig.sentimentModel = geminiJson.value("sentiment_model", geminiConfig.sentimentModel);
    geminiConfig.visionModel = geminiJson.value("vision_model", geminiConfig.visionModel);
    const auto modelResolutionJson = geminiJson.value("model_resolution", nlohmann::json::object());
    geminiConfig.modelResolutionEnabled =
        modelResolutionJson.value("enabled", geminiConfig.modelResolutionEnabled);
    geminiConfig.modelResolutionMode =
        modelResolutionJson.value("mode", geminiConfig.modelResolutionMode);
    geminiConfig.modelResolutionFallbackOnError =
        modelResolutionJson.value("fallback_on_error", geminiConfig.modelResolutionFallbackOnError);
    geminiConfig.modelResolutionAllowPreview =
        modelResolutionJson.value("allow_preview", geminiConfig.modelResolutionAllowPreview);
    geminiConfig.sentimentSearchThenScore =
        geminiJson.value("sentiment_search_then_score", geminiConfig.sentimentSearchThenScore);
    geminiConfig.sentimentWeight = geminiJson.value("sentiment_weight", geminiConfig.sentimentWeight);
    geminiConfig.visionWeight = geminiJson.value("vision_weight", geminiConfig.visionWeight);
    geminiConfig.confidenceThreshold = geminiJson.value("confidence_threshold", geminiConfig.confidenceThreshold);
    geminiConfig.timeoutSeconds = geminiJson.value("timeout_seconds", geminiConfig.timeoutSeconds);
    geminiConfig.maxEvaluationsPerScanCycle =
        geminiJson.value("max_evaluations_per_scan_cycle", geminiConfig.maxEvaluationsPerScanCycle);
    geminiConfig.staleRuntimeTtlHours =
        geminiJson.value("stale_runtime_ttl_hours", geminiConfig.staleRuntimeTtlHours);
    geminiConfig.resultCacheTtlSeconds =
        geminiJson.value("result_cache_ttl_seconds", geminiConfig.resultCacheTtlSeconds);
    geminiConfig.sentimentCacheTtlSeconds =
        geminiJson.value("sentiment_cache_ttl_seconds", geminiConfig.sentimentCacheTtlSeconds);
    geminiConfig.sentimentCacheMaxStaleSeconds =
        geminiJson.value("sentiment_cache_max_stale_seconds", geminiConfig.sentimentCacheMaxStaleSeconds);
    geminiConfig.modelResolutionTtlSeconds =
        geminiJson.value("model_resolution_ttl_seconds", geminiConfig.modelResolutionTtlSeconds);
    geminiConfig.modelResolutionMaxStaleSeconds =
        geminiJson.value("model_resolution_max_stale_seconds", geminiConfig.modelResolutionMaxStaleSeconds);
    geminiConfig.blockOnError = geminiJson.value("block_on_error", geminiConfig.blockOnError);
    geminiConfig.blockOnBudgetExhausted =
        geminiJson.value("block_on_budget_exhausted", geminiConfig.blockOnBudgetExhausted);
    geminiConfig.closeGateOnBudgetExhausted =
        geminiJson.value("close_gate_on_budget_exhausted", geminiConfig.closeGateOnBudgetExhausted);
    geminiConfig.closeGateOnQuotaExhausted =
        geminiJson.value("close_gate_on_quota_exhausted", geminiConfig.closeGateOnQuotaExhausted);
    const auto modelRoutingJson = geminiJson.value("model_routing", nlohmann::json::object());
    geminiConfig.modelRoutingEnabled = modelRoutingJson.value("enabled", geminiConfig.modelRoutingEnabled);
    const auto modelRoutingSentimentJson = modelRoutingJson.value("sentiment", nlohmann::json::object());
    geminiConfig.sentimentModelCandidates =
        modelRoutingSentimentJson.value("candidates", geminiConfig.sentimentModelCandidates);
    const auto modelRoutingVisionJson = modelRoutingJson.value("vision", nlohmann::json::object());
    geminiConfig.visionModelCandidates =
        modelRoutingVisionJson.value("candidates", geminiConfig.visionModelCandidates);
    geminiConfig.visionProEscalationEnabled =
        modelRoutingVisionJson.value("pro_escalation_enabled", geminiConfig.visionProEscalationEnabled);
    geminiConfig.visionProEscalationMinScore =
        modelRoutingVisionJson.value("pro_escalation_min_score", geminiConfig.visionProEscalationMinScore);
    geminiConfig.visionProEscalationMaxScore =
        modelRoutingVisionJson.value("pro_escalation_max_score", geminiConfig.visionProEscalationMaxScore);
    const auto quotaJson = geminiJson.value("quota", nlohmann::json::object());
    geminiConfig.quotaEnabled = quotaJson.value("enabled", geminiConfig.quotaEnabled);
    geminiConfig.quotaSafetyFactor = quotaJson.value("safety_factor", geminiConfig.quotaSafetyFactor);
    geminiConfig.quotaCooldownSecondsOn429 =
        quotaJson.value("cooldown_seconds_on_429", geminiConfig.quotaCooldownSecondsOn429);
    geminiConfig.quotaDefaultRpm = quotaJson.value("default_rpm", geminiConfig.quotaDefaultRpm);
    geminiConfig.quotaDefaultRpd = quotaJson.value("default_rpd", geminiConfig.quotaDefaultRpd);
    geminiConfig.quotaModelLimits.clear();
    if (quotaJson.contains("models") && quotaJson.at("models").is_object()) {
        for (const auto& [modelName, value] : quotaJson.at("models").items()) {
            if (!value.is_object()) {
                continue;
            }
            const int rpm = value.value("rpm", 0);
            const int rpd = value.value("rpd", 0);
            if (rpm <= 0 || rpd <= 0) {
                continue;
            }
            geminiConfig.quotaModelLimits.push_back(engine::GeminiFilterConfig::QuotaModelLimit{
                .model = modelName,
                .rpm = rpm,
                .rpd = rpd,
            });
        }
    }
    geminiConfig.extraTfs = geminiJson.value("extra_tfs", geminiConfig.extraTfs);

    if (!geminiConfig.enabled) {
        geminiConfig.mode = engine::GeminiFilterMode::Disabled;
    }
    if (geminiConfig.timeoutSeconds <= 0) {
        Logger::instance().log(
            LogLevel::Warning,
            "gemini timeout_seconds is invalid; force to 10");
        geminiConfig.timeoutSeconds = 10;
    }
    if (geminiConfig.maxEvaluationsPerScanCycle < 0) {
        Logger::instance().log(
            LogLevel::Warning,
            "gemini max_evaluations_per_scan_cycle is invalid; force to 0");
        geminiConfig.maxEvaluationsPerScanCycle = 0;
    }
    if (geminiConfig.sentimentWeight < 0.0 || geminiConfig.visionWeight < 0.0 ||
        (geminiConfig.sentimentWeight + geminiConfig.visionWeight) <= 0.0) {
        Logger::instance().log(
            LogLevel::Warning,
            "gemini weights are invalid; fallback to 0.5/0.5");
        geminiConfig.sentimentWeight = 0.5;
        geminiConfig.visionWeight = 0.5;
    }
    geminiConfig.confidenceThreshold = std::clamp(geminiConfig.confidenceThreshold, 0.0, 1.0);
    geminiConfig.visionProEscalationMinScore = std::clamp(geminiConfig.visionProEscalationMinScore, 0.0, 1.0);
    geminiConfig.visionProEscalationMaxScore = std::clamp(geminiConfig.visionProEscalationMaxScore, 0.0, 1.0);
    if (geminiConfig.visionProEscalationMinScore > geminiConfig.visionProEscalationMaxScore) {
        std::swap(geminiConfig.visionProEscalationMinScore, geminiConfig.visionProEscalationMaxScore);
    }
    if (geminiConfig.quotaSafetyFactor <= 0.0) {
        geminiConfig.quotaSafetyFactor = 0.7;
    }
    geminiConfig.quotaSafetyFactor = std::min(1.0, geminiConfig.quotaSafetyFactor);
    if (geminiConfig.quotaDefaultRpm <= 0) {
        geminiConfig.quotaDefaultRpm = 8;
    }
    if (geminiConfig.quotaDefaultRpd <= 0) {
        geminiConfig.quotaDefaultRpd = 250;
    }

    const std::unordered_set<std::string> scannerIntervalSet(scannerIntervals.begin(), scannerIntervals.end());
    for (const auto& tf : geminiConfig.extraTfs) {
        if (scannerIntervalSet.find(tf) == scannerIntervalSet.end()) {
            Logger::instance().log(
                LogLevel::Warning,
                "gemini extra_tf not present in scanner intervals tf=" + quoteString(tf));
        }
    }

    engine::RiskConfig riskConfig;
    try {
        riskConfig = engine::RiskConfig::fromJson(riskJson);
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::Error, std::string("Invalid risk_analytics config: ") + e.what());
        return 1;
    }

    ContextConfig contextConfig;
    contextConfig.apiKey = apiKey;
    contextConfig.secretKey = secretKey;
    contextConfig.testnet = std::getenv("BINANCE_TESTNET") != nullptr;
    contextConfig.threadPoolSize = 2;

    std::string proxySourceName;
    const char* proxyRaw = nullptr;
    if (const char* explicitProxy = std::getenv("BINANCE_SOCKS5_PROXY"); explicitProxy && *explicitProxy) {
        proxySourceName = "BINANCE_SOCKS5_PROXY";
        proxyRaw = explicitProxy;
    } else if (const char* allProxy = std::getenv("ALL_PROXY"); allProxy && *allProxy) {
        proxySourceName = "ALL_PROXY";
        proxyRaw = allProxy;
    } else if (const char* allProxyLower = std::getenv("all_proxy"); allProxyLower && *allProxyLower) {
        proxySourceName = "all_proxy";
        proxyRaw = allProxyLower;
    }

    if (proxyRaw) {
        std::string proxyError;
        auto proxy = parseSocks5Proxy(proxyRaw, proxyError);
        if (!proxy && !proxyError.empty()) {
            Logger::instance().log(LogLevel::Error, "Invalid SOCKS5 proxy from " + proxySourceName + ": " + proxyError);
            return 1;
        }
        if (proxy) {
            contextConfig.socks5Proxy = *proxy;
            Logger::instance().log(
                LogLevel::Info,
                "Using SOCKS5 proxy from " + proxySourceName + ": " + contextConfig.socks5Proxy.host + ":"
                    + std::to_string(contextConfig.socks5Proxy.port));
        }
    }
    if (!contextConfig.socks5Proxy.enabled()) {
        if (std::getenv("BINANCE_REQUIRE_SOCKS5_PROXY") != nullptr) {
            Logger::instance().log(
                LogLevel::Error,
                "BINANCE_REQUIRE_SOCKS5_PROXY is set but no SOCKS5 proxy was configured. "
                "Set BINANCE_SOCKS5_PROXY=socks5://127.0.0.1:1080 after opening the EC2 tunnel.");
            return 1;
        }
        Logger::instance().log(
            LogLevel::Warning,
            "SOCKS5 proxy is not configured; Binance REST and WebSocket traffic will use the local network egress IP");
    }

    BinanceContext ctx(contextConfig);
    RestClient rest = ctx.makeRestClient();

    auto ping = syncAwait(ctx.ioc(), rest.ping());
    if (!ping || !*ping) {
        const std::string detail = ping ? "" : ": " + ping.error().toString();
        Logger::instance().log(LogLevel::Error, "Failed to connect to Binance REST API" + detail);
        return 1;
    }

    scanner::MarketScanner scanner(
        rest,
        ctx,
        scanner::MarketScanner::Config{
            .intervals = scannerIntervals,
            .klineBufferSize = scannerBufferSize,
            .maxStreamsPerConnection = static_cast<size_t>(scannerJson.value("max_streams_per_connection", 512)),
            .warmupRequestDelay = std::chrono::milliseconds(scannerJson.value("warmup_request_delay_ms", 0)),
            .warmupInitialLimit = warmupInitialLimit,
            .warmupConcurrency = warmupConcurrency,
            .backfillEnabled = backfillEnabled,
            .backfillConcurrency = backfillConcurrency,
            .backfillRequestDelay = std::chrono::milliseconds(scannerJson.value("backfill_request_delay_ms", 200)),
            .betaDailyKlinesEnabled = betaDailyEnabled,
            .betaDailyInterval = betaDailyInterval,
            .betaDailyLimit = betaDailyLimit,
        });

    auto scannerStart = syncAwait(ctx.ioc(), scanner.start());
    if (!scannerStart) {
        Logger::instance().log(LogLevel::Error, "MarketScanner start failed: " + scannerStart.error().toString());
        return 1;
    }

    RestClientAdapter ordersRest(rest);
    Orders orders(
        ordersRest,
        OrdersConfig{
            .clientIdNamespace = "bot",
            .allowBestEffortJournal = true,
            .positionMode = PositionMode::OneWay,
        });

    account::AccountService accountSvc(rest, account::AccountCompatibilityConfig{});
    engine::ScannerPort scannerPort(scanner);
    engine::AccountPort accountPort(accountSvc);
    engine::OrdersPort ordersPort(orders);
    engine::NoOpExposurePort noOpExposure;
    std::unique_ptr<engine::ExposureController> exposureController;
    engine::IExposurePort* exposurePort = &noOpExposure;
    if (exposureConfig.enabled) {
        exposureController = std::make_unique<engine::ExposureController>(exposureConfig, scanner.cache());
        exposurePort = exposureController.get();
    }

    engine::NoOpOrderCapPort noOpOrderCap;
    std::unique_ptr<engine::TotalNotionalGuard> orderCapController;
    engine::IOrderCapPort* orderCapPort = &noOpOrderCap;
    if (orderCapConfig.enabled) {
        orderCapController = std::make_unique<engine::TotalNotionalGuard>(orderCapConfig);
        orderCapPort = orderCapController.get();
    }

    engine::NoOpGeminiFilterPort noOpGemini;
    std::unique_ptr<engine::GeminiFilterController> geminiController;
    engine::IGeminiFilterPort* geminiPort = &noOpGemini;
    if (geminiConfig.enabled && geminiConfig.mode != engine::GeminiFilterMode::Disabled) {
        try {
            geminiController = std::make_unique<engine::GeminiFilterController>(geminiConfig);
            geminiPort = geminiController.get();
            Logger::instance().log(
                LogLevel::Info,
                std::string("Gemini filter enabled mode=enforce") +
                    " python=" + quoteString(geminiConfig.pythonPath) +
                    " module=" + quoteString(geminiConfig.moduleName) +
                    " model_resolution=" +
                    (geminiConfig.modelResolutionEnabled ? geminiConfig.modelResolutionMode : "pinned") +
                    " timeout_seconds=" + std::to_string(geminiConfig.timeoutSeconds) +
                    " max_evaluations_per_scan_cycle=" +
                    std::to_string(geminiConfig.maxEvaluationsPerScanCycle) +
                    " result_cache_ttl_seconds=" + std::to_string(geminiConfig.resultCacheTtlSeconds) +
                    " sentiment_cache_ttl_seconds=" + std::to_string(geminiConfig.sentimentCacheTtlSeconds) +
                    " model_resolution_ttl_seconds=" + std::to_string(geminiConfig.modelResolutionTtlSeconds) +
                    " block_on_error=" + (geminiConfig.blockOnError ? "true" : "false") +
                    " block_on_budget_exhausted=" + (geminiConfig.blockOnBudgetExhausted ? "true" : "false") +
                    " close_gate_on_budget_exhausted=" +
                    (geminiConfig.closeGateOnBudgetExhausted ? "true" : "false") +
                    " close_gate_on_quota_exhausted=" +
                    (geminiConfig.closeGateOnQuotaExhausted ? "true" : "false") +
                    " model_routing=" + (geminiConfig.modelRoutingEnabled ? "enabled" : "disabled") +
                    " quota=" + (geminiConfig.quotaEnabled ? "enabled" : "disabled"));
        } catch (const std::exception& e) {
            Logger::instance().log(LogLevel::Error, std::string("Gemini filter init failed: ") + e.what());
            return 1;
        }
    } else {
        Logger::instance().log(LogLevel::Info, "Gemini filter disabled");
    }

    engine::NoOpRiskPort noOpRisk;
    std::unique_ptr<engine::RiskDb> riskDb;
    std::unique_ptr<engine::EquityCurve> riskCurve;
    std::unique_ptr<engine::RiskMetrics> riskMetrics;
    std::unique_ptr<engine::RiskController> riskController;
    engine::IRiskPort* riskPort = &noOpRisk;

    if (riskConfig.enabled) {
        try {
            riskDb = std::make_unique<engine::RiskDb>(riskConfig.dbPath);
            riskCurve = std::make_unique<engine::EquityCurve>(*riskDb);
            riskMetrics = std::make_unique<engine::RiskMetrics>(
                riskConfig.riskFreeRate,
                riskConfig.minDataPoints,
                std::chrono::minutes{riskConfig.sampleIntervalMinutes});
            riskController = std::make_unique<engine::RiskController>(
                *riskDb,
                *riskCurve,
                *riskMetrics,
                riskConfig);
            riskPort = riskController.get();
            Logger::instance().log(
                LogLevel::Info,
                "Risk analytics enabled db_path=" + quoteString(riskConfig.dbPath) +
                    " basis=" + quoteString(engine::toString(riskConfig.equityBasis)) +
                    " lookback_days=" + std::to_string(riskConfig.controlLookbackDays) +
                    " sample_interval_minutes=" + std::to_string(riskConfig.sampleIntervalMinutes));
        } catch (const std::exception& e) {
            Logger::instance().log(LogLevel::Error, std::string("Risk analytics init failed: ") + e.what());
            return 1;
        }
    } else {
        Logger::instance().log(LogLevel::Info, "Risk analytics disabled");
    }

    engine::SignalEngine signalEngine(
        scannerPort,
        registry,
        accountPort,
        ordersPort,
        *orderCapPort,
        *exposurePort,
        *geminiPort,
        geminiConfig,
        engine::SignalEngine::Config{
            .minNotional = engineJson.value("min_notional", 1.0),
            .maxPositionNotionalXAvailableBalance =
                engineJson.value("max_position_notional_x_available_balance", 0.5),
            .positionCheckInterval = std::chrono::seconds(engineJson.value("position_check_interval_seconds", 60)),
            .trailingCheckInterval = std::chrono::seconds(engineJson.value("trailing_check_interval_seconds", 300)),
            .placeStopLoss = engineJson.value("place_stop_loss", true),
            .monitorTrailingStops = engineJson.value("monitor_trailing_stops", true),
        },
        riskPort);
    signalEngine.setScanCycleStatusCallback(
        [&strategyCatalog](int queueItems, int openPositions) {
            catalog::CatalogReporter::logRuntimeStatus(
                strategyCatalog.listStrategies(),
                queueItems,
                openPositions);
        });

    UserDataStream userData = ctx.makeUserDataStream();
    userData.start([&signalEngine](boost::system::error_code ec, UserDataEvent event) {
        if (ec) {
            Logger::instance().log(LogLevel::Warning, "User data stream error: " + ec.message());
            return;
        }
        signalEngine.onUserDataEvent(event);
    });

    boost::asio::co_spawn(
        ctx.ioc(),
        [&signalEngine]() -> boost::asio::awaitable<void> { co_await signalEngine.run(); },
        [](std::exception_ptr ep) {
            if (!ep) {
                return;
            }
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                Logger::instance().log(LogLevel::Error, std::string("SignalEngine coroutine exception: ") + e.what());
            } catch (...) {
                Logger::instance().log(LogLevel::Error, "SignalEngine coroutine unknown exception");
            }
            g_running = false;
        });

    Logger::instance().log(LogLevel::Info, "SignalEngine started. Press Ctrl+C to stop");
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    signalEngine.stop();
    userData.stop();
    scanner.stop();

    Logger::instance().log(LogLevel::Info, "Binance Trading Bot stopped");
    return 0;
}
