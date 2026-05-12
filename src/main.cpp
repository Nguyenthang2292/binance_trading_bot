#include "binance_api.h"
#include "trading_engine.h"
#include "logger.h"
#include <iostream>
#include <csignal>
#include <chrono>
#include <thread>

std::atomic<bool> g_running{true};

void signalHandler(int signum) {
    Logger::instance().log(LogLevel::INFO, "Received signal " + std::to_string(signum) + ", shutting down...");
    g_running = false;
}

void onSignal(const Signal& signal) {
    std::string action;
    switch (signal.action) {
        case Signal::Action::BUY:  action = "BUY"; break;
        case Signal::Action::SELL: action = "SELL"; break;
        case Signal::Action::HOLD: action = "HOLD"; break;
    }
    Logger::instance().log(LogLevel::TRADE,
        "Signal received: " + action + " (confidence: " + std::to_string(signal.confidence) + ")");
}

void onTrade(const NormalPlacementResult& order) {
    const auto state =
        order.state == PlacementState::Accepted
            ? "ACCEPTED"
            : (order.state == PlacementState::Rejected ? "REJECTED" : "UNKNOWN_PENDING_RECONCILE");
    Logger::instance().log(LogLevel::TRADE,
        "Trade executed: " + order.symbol +
        " state=" + state +
        " clientOrderId=" + order.clientOrderId +
        " orderId=" + std::to_string(order.orderId.value_or(0)) +
        " status=" + order.orderStatus.value_or("N/A"));
}

int main(int argc, char* argv[]) {
    std::cout << R"(
    ____  _                         __          _             _   ____        _      __
   / __ )(_)___  ____ _____  ___   / /_ _____ _(_)___  ___   / /  / __ )____  / /_   / /
  / __  / / __ \/ __ `/ __ \/ _ \ / / / / __ `/ / __ \/ _ \ / /  / __  / __ \/ __/  / /
 / /_/ / / / / / /_/ / / / /  __// / /_/ / /_/ / / / / /  __// /  / /_/ / /_/ / /_   /_/
/_____/_/_/ /_/\__,_/_/ /_/\___//_/\__,_/\__,_/_/_/ /_/\___//_/  /_____/\____/\__/  (_)
)" << std::endl;

    Logger::instance().setLogFile("trading_bot.log");
    Logger::instance().setMinLevel(LogLevel::DEBUG);
    Logger::instance().log(LogLevel::INFO, "Binance Trading Bot v1.0.0 started");

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    const char* apiKey = std::getenv("BINANCE_API_KEY");
    const char* secretKey = std::getenv("BINANCE_SECRET_KEY");

    if (!apiKey || !secretKey) {
        Logger::instance().log(LogLevel::ERROR,
            "Please set BINANCE_API_KEY and BINANCE_SECRET_KEY environment variables");
        return 1;
    }

    BinanceAPI api(apiKey, secretKey);

    Logger::instance().log(LogLevel::INFO, "Testing Binance connectivity...");
    if (!api.testConnectivity()) {
        Logger::instance().log(LogLevel::ERROR, "Failed to connect to Binance API");
        return 1;
    }
    Logger::instance().log(LogLevel::INFO, "Connected to Binance API successfully");

    auto btcPrice = api.getPrice("BTCUSDT");
    if (btcPrice.has_value()) {
        Logger::instance().log(LogLevel::INFO,
            "BTC/USDT Current Price: $" + std::to_string(btcPrice.value()));
    }

    TradingConfig config;
    config.symbol = "BTCUSDT";
    config.interval = "15m";
    config.tradeQuantity = 0.001;
    config.stopLossPercent = 2.0;
    config.takeProfitPercent = 4.0;
    config.rsiPeriod = 14;
    config.rsiOversold = 30.0;
    config.rsiOverbought = 70.0;
    config.smaShortPeriod = 9;
    config.smaLongPeriod = 21;
    config.pollIntervalMs = 60000;

    TradingEngine engine(api, config);
    engine.setOnSignal(onSignal);
    engine.setOnTrade(onTrade);

    Logger::instance().log(LogLevel::INFO, "Starting trading engine (DRY RUN - no real trades)");
    Logger::instance().log(LogLevel::INFO, "Symbol: " + config.symbol + " Interval: " + config.interval);
    Logger::instance().log(LogLevel::INFO, "Press Ctrl+C to stop");

    engine.start();

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    Logger::instance().log(LogLevel::INFO, "Stopping trading engine...");
    engine.stop();
    Logger::instance().log(LogLevel::INFO, "Binance Trading Bot stopped");

    return 0;
}
