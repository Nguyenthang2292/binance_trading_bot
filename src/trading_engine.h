#pragma once

#include <string>
#include <vector>
#include <optional>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include "binance_api.h"

struct TradingConfig {
    std::string symbol;
    std::string interval;
    double tradeQuantity;
    double stopLossPercent;
    double takeProfitPercent;
    int rsiPeriod;
    double rsiOversold;
    double rsiOverbought;
    int smaShortPeriod;
    int smaLongPeriod;
    int pollIntervalMs;
};

struct Signal {
    enum class Action { BUY, SELL, HOLD };
    Action action;
    std::string reason;
    double confidence;
};

class TradingEngine {
public:
    TradingEngine(BinanceAPI& api, const TradingConfig& config);
    ~TradingEngine();

    void start();
    void stop();
    bool isRunning() const;

    std::vector<Kline> getHistoricalData(size_t count);
    double calculateRSI(const std::vector<double>& closes, int period = 14);
    double calculateSMA(const std::vector<double>& prices, int period);
    double calculateEMA(const std::vector<double>& prices, int period);

    Signal analyzeMarket(const std::vector<Kline>& klines);

    void setOnSignal(std::function<void(const Signal&)> callback);
    void setOnTrade(std::function<void(const NormalPlacementResult&)> callback);

private:
    BinanceAPI& m_api;
    TradingConfig m_config;
    std::atomic<bool> m_running{false};
    std::thread m_workerThread;

    std::function<void(const Signal&)> m_onSignal;
    std::function<void(const NormalPlacementResult&)> m_onTrade;

    void tradingLoop();
    void executeSignal(const Signal& signal);
    bool hasOpenPosition(const std::string& symbol);
};
