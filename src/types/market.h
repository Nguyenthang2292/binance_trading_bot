#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct Kline {
    int64_t openTime{0};
    int64_t closeTime{0};
    double open{0.0};
    double high{0.0};
    double low{0.0};
    double close{0.0};
    double volume{0.0};
    double quoteVolume{0.0};
    int32_t tradeCount{0};
    bool isClosed{false};

    // Backward-compatible names used by the existing trading engine tests.
    double quoteAssetVolume{0.0};
    int32_t numberOfTrades{0};
};

struct OrderBook {
    int64_t lastUpdateId{0};
    std::vector<std::pair<double, double>> bids;
    std::vector<std::pair<double, double>> asks;
};

struct Trade {
    int64_t id{0};
    int64_t time{0};
    std::string symbol;
    double price{0.0};
    double qty{0.0};
    bool isBuyerMaker{false};
};

struct Ticker24h {
    std::string symbol;
    double lastPrice{0.0};
    double priceChange{0.0};
    double priceChangePercent{0.0};
    double highPrice{0.0};
    double lowPrice{0.0};
    double volume{0.0};
    double quoteVolume{0.0};
    int64_t openTime{0};
    int64_t closeTime{0};
};

using Ticker = Ticker24h;

struct MarkPrice {
    std::string symbol;
    double markPrice{0.0};
    double indexPrice{0.0};
    double estimatedSettlePrice{0.0};
    double fundingRate{0.0};
    int64_t nextFundingTime{0};
    int64_t time{0};
};

struct ExchangeSymbol {
    std::string symbol;
    std::string baseAsset;
    std::string quoteAsset;
    std::string contractType;
    std::string status;
    int pricePrecision{0};
    int quantityPrecision{0};
    int baseAssetPrecision{0};
    double tickSize{0.0};
    double stepSize{0.0};
    double minNotional{0.0};
    double maxQty{0.0};
    double minQty{0.0};
};
