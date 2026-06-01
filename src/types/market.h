#pragma once

#include <cstdint>
#include <optional>
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
    std::string lastPriceRaw{"0"};
    double priceChange{0.0};
    std::string priceChangeRaw{"0"};
    double priceChangePercent{0.0};
    std::string priceChangePercentRaw{"0"};
    double highPrice{0.0};
    std::string highPriceRaw{"0"};
    double lowPrice{0.0};
    std::string lowPriceRaw{"0"};
    double volume{0.0};
    std::string volumeRaw{"0"};
    double quoteVolume{0.0};
    std::string quoteVolumeRaw{"0"};
    int64_t openTime{0};
    int64_t closeTime{0};
};

using Ticker = Ticker24h;

struct MarkPrice {
    std::string symbol;
    double markPrice{0.0};
    std::string markPriceRaw{"0"};
    double indexPrice{0.0};
    std::string indexPriceRaw{"0"};
    double estimatedSettlePrice{0.0};
    std::string estimatedSettlePriceRaw{"0"};
    double fundingRate{0.0};
    std::string fundingRateRaw{"0"};
    int64_t nextFundingTime{0};
    int64_t time{0};
};

struct ExchangePriceFilter {
    double minPrice{0.0};
    double maxPrice{0.0};
    double tickSize{0.0};
    // WR-34: exact decimal strings as sent by the exchange. A binary double
    // cannot represent decimal ticks exactly, so the raw string is the source of
    // truth for tick-decimal counting; the double is a convenience for math.
    std::string minPriceRaw{"0"};
    std::string maxPriceRaw{"0"};
    std::string tickSizeRaw{"0"};
};

struct ExchangeLotSizeFilter {
    double minQty{0.0};
    double maxQty{0.0};
    double stepSize{0.0};
    std::string minQtyRaw{"0"};
    std::string maxQtyRaw{"0"};
    std::string stepSizeRaw{"0"};
};

struct ExchangeNotionalFilter {
    double minNotional{0.0};
    double maxNotional{0.0};
    bool applyMinToMarket{false};
    bool applyMaxToMarket{false};
    int avgPriceMins{0};
    std::string minNotionalRaw{"0"};
    std::string maxNotionalRaw{"0"};
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

    std::optional<ExchangePriceFilter> priceFilter;
    std::optional<ExchangeLotSizeFilter> lotSize;
    std::optional<ExchangeLotSizeFilter> marketLotSize;
    std::optional<ExchangeNotionalFilter> minNotionalFilter;
    std::optional<ExchangeNotionalFilter> notionalFilter;

    // Legacy aliases retained for existing sizing/validation code.
    double tickSize{0.0};
    double stepSize{0.0};
    double minNotional{0.0};
    double maxQty{0.0};
    double minQty{0.0};

    // WR-34: exact decimal strings mirroring the legacy double aliases above,
    // used for precise tick/step decimal counting at order-formatting time.
    std::string tickSizeRaw{"0"};
    std::string stepSizeRaw{"0"};
    std::string minNotionalRaw{"0"};
    std::string maxQtyRaw{"0"};
    std::string minQtyRaw{"0"};
};
