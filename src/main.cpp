#include "account/account_service.h"
#include "catalog/catalog_reporter.h"
#include "catalog/plugin_loader.h"
#include "catalog/strategy_catalog.h"
#include "context.h"
#include "engine/exposure_controller.h"
#include "engine/signal_engine.h"
#include "logger.h"
#include "orders/orders.h"
#include "orders/rest_client_adapter.h"
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
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

std::atomic<bool> g_running{true};

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

int main(int argc, char* argv[]) {
    Logger::instance().setLogFile("trading_bot.log");
    Logger::instance().setMinLevel(LogLevel::Info);
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

    const auto pluginsDir = config.value("catalog", nlohmann::json::object()).value("plugins_dir", "plugins");
    strategy::StrategyRegistry registry;
    catalog::PluginLoader pluginLoader({.pluginsDir = pluginsDir});
    catalog::StrategyCatalog strategyCatalog({.pluginsDir = pluginsDir}, registry, std::move(pluginLoader));
    const auto catalogSummary = strategyCatalog.initialize(toStrategyConfigs(config));

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

    const bool betaDailyEnabled = scannerJson.value("beta_daily_klines_enabled", exposureConfig.enabled);
    const auto betaDailyInterval = scannerJson.value("beta_daily_interval", std::string("1d"));
    const int betaDailyLimit =
        scannerJson.value("beta_daily_limit", std::max(2, exposureConfig.betaWindowDays + 1));

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

    BinanceContext ctx(contextConfig);
    RestClient rest = ctx.makeRestClient();

    auto ping = syncAwait(ctx.ioc(), rest.ping());
    if (!ping || !*ping) {
        Logger::instance().log(LogLevel::Error, "Failed to connect to Binance REST API");
        return 1;
    }

    scanner::MarketScanner scanner(
        rest,
        ctx,
        scanner::MarketScanner::Config{
            .intervals = scannerJson.value("intervals", std::vector<std::string>{"15m", "30m"}),
            .klineBufferSize = static_cast<size_t>(scannerJson.value("kline_buffer_size", 200)),
            .maxStreamsPerConnection = static_cast<size_t>(scannerJson.value("max_streams_per_connection", 512)),
            .warmupRequestDelay = std::chrono::milliseconds(scannerJson.value("warmup_request_delay_ms", 100)),
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
    engine::SignalEngine signalEngine(
        scannerPort,
        registry,
        accountPort,
        ordersPort,
        *exposurePort,
        engine::SignalEngine::Config{
            .minNotional = engineJson.value("min_notional", 1.0),
            .positionCheckInterval = std::chrono::seconds(engineJson.value("position_check_interval_seconds", 60)),
            .trailingCheckInterval = std::chrono::seconds(engineJson.value("trailing_check_interval_seconds", 300)),
            .placeStopLoss = engineJson.value("place_stop_loss", true),
            .monitorTrailingStops = engineJson.value("monitor_trailing_stops", true),
        });
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
        boost::asio::detached);

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
