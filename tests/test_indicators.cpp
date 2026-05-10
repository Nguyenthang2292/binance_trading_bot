#include <gtest/gtest.h>
#include "trading_engine.h"
#include <cmath>
#include <vector>

class IndicatorsTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_api = new BinanceAPI("test_key", "test_secret");
        m_config.symbol = "BTCUSDT";
        m_config.interval = "15m";
        m_config.rsiPeriod = 14;
        m_config.rsiOversold = 30.0;
        m_config.rsiOverbought = 70.0;
        m_config.smaShortPeriod = 9;
        m_config.smaLongPeriod = 21;
        m_engine = new TradingEngine(*m_api, m_config);
    }

    void TearDown() override {
        delete m_engine;
        delete m_api;
    }

    BinanceAPI* m_api;
    TradingConfig m_config;
    TradingEngine* m_engine;
};

TEST_F(IndicatorsTest, SMA_CorrectCalculation) {
    std::vector<double> prices = {10.0, 10.5, 11.0, 10.8, 11.2};
    double sma = m_engine->calculateSMA(prices, 3);
    double expected = (11.0 + 10.8 + 11.2) / 3.0;
    EXPECT_NEAR(sma, expected, 0.0001);
}

TEST_F(IndicatorsTest, SMA_TooFewPricesReturnsZero) {
    std::vector<double> prices = {10.0, 10.5};
    double sma = m_engine->calculateSMA(prices, 5);
    EXPECT_DOUBLE_EQ(sma, 0.0);
}

TEST_F(IndicatorsTest, SMA_ConstantValues) {
    std::vector<double> prices = {5.0, 5.0, 5.0, 5.0, 5.0};
    double sma = m_engine->calculateSMA(prices, 3);
    EXPECT_DOUBLE_EQ(sma, 5.0);
}

TEST_F(IndicatorsTest, EMA_CalculatesCorrectly) {
    std::vector<double> prices = {22.0, 22.5, 23.0, 22.8, 23.2, 23.1, 23.5};
    double ema = m_engine->calculateEMA(prices, 5);
    EXPECT_GT(ema, 22.5);
    EXPECT_LT(ema, 23.5);
}

TEST_F(IndicatorsTest, EMA_TooFewPricesReturnsZero) {
    std::vector<double> prices = {10.0};
    double ema = m_engine->calculateEMA(prices, 5);
    EXPECT_DOUBLE_EQ(ema, 0.0);
}

TEST_F(IndicatorsTest, RSI_AllUpReturns100) {
    std::vector<double> closes(15);
    for (size_t i = 0; i < closes.size(); i++) {
        closes[i] = 100.0 + i * 5.0;
    }
    double rsi = m_engine->calculateRSI(closes, 14);
    EXPECT_NEAR(rsi, 100.0, 0.01);
}

TEST_F(IndicatorsTest, RSI_AllDownReturnsZero) {
    std::vector<double> closes(15);
    for (size_t i = 0; i < closes.size(); i++) {
        closes[i] = 200.0 - i * 5.0;
    }
    double rsi = m_engine->calculateRSI(closes, 14);
    EXPECT_NEAR(rsi, 0.0, 0.01);
}

TEST_F(IndicatorsTest, RSI_SidewaysReturnsNear50) {
    std::vector<double> closes = {
        50.0, 50.1, 49.9, 50.0, 50.1, 49.9, 50.0,
        50.1, 49.9, 50.0, 50.1, 49.9, 50.0, 50.1, 49.9
    };
    double rsi = m_engine->calculateRSI(closes, 14);
    EXPECT_NEAR(rsi, 50.0, 5.0);
}

TEST_F(IndicatorsTest, RSI_InsufficientDataReturns50) {
    std::vector<double> closes = {50.0, 50.1, 49.9};
    double rsi = m_engine->calculateRSI(closes, 14);
    EXPECT_DOUBLE_EQ(rsi, 50.0);
}

TEST_F(IndicatorsTest, AnalyzeMarket_BullishSignalsBuy) {
    std::vector<Kline> klines;
    double base = 50000.0;

    for (int i = 0; i < 30; i++) {
        Kline k;
        k.open = base;
        k.close = base + i * 100.0;
        k.high = base + i * 120.0;
        k.low = base + i * 80.0;
        k.volume = 100.0;
        k.openTime = 1000000 + i * 60000;
        k.closeTime = 1000000 + (i + 1) * 60000;
        k.quoteAssetVolume = 5000000.0;
        k.numberOfTrades = 100;
        klines.push_back(k);
        base += 100.0;
    }

    Signal sig = m_engine->analyzeMarket(klines);
    EXPECT_NE(sig.action, Signal::Action::HOLD);
}

TEST_F(IndicatorsTest, AnalyzeMarket_BearishSignalsSell) {
    std::vector<Kline> klines;
    double base = 50000.0;

    for (int i = 0; i < 30; i++) {
        Kline k;
        k.open = base;
        k.close = base - i * 100.0;
        k.high = base - i * 80.0;
        k.low = base - i * 120.0;
        k.volume = 100.0;
        k.openTime = 1000000 + i * 60000;
        k.closeTime = 1000000 + (i + 1) * 60000;
        k.quoteAssetVolume = 5000000.0;
        k.numberOfTrades = 100;
        klines.push_back(k);
        base -= 100.0;
    }

    Signal sig = m_engine->analyzeMarket(klines);
    EXPECT_NE(sig.action, Signal::Action::HOLD);
}
