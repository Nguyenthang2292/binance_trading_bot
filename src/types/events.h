#pragma once

#include "types/account.h"
#include "types/market.h"
#include "types/trade.h"

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

struct AggTradeEvent {
    std::string symbol;
    int64_t aggTradeId{0};
    int64_t time{0};
    double price{0.0};
    double qty{0.0};
    bool isBuyerMaker{false};
};

struct KlineEvent {
    std::string symbol;
    Kline kline;
    std::string interval;
};

struct MarkPriceEvent {
    std::string symbol;
    double markPrice{0.0};
    double indexPrice{0.0};
    double estimatedSettlePrice{0.0};
    double fundingRate{0.0};
    int64_t nextFundingTime{0};
    int64_t time{0};
};

struct BookTickerEvent {
    std::string symbol;
    int64_t updateId{0};
    double bidPrice{0.0};
    double bidQty{0.0};
    double askPrice{0.0};
    double askQty{0.0};
    int64_t eventTime{0};
    int64_t transactTime{0};
};

struct DepthEvent {
    std::string symbol;
    int64_t firstUpdateId{0};
    int64_t finalUpdateId{0};
    int64_t prevFinalUpdateId{0};
    std::vector<std::pair<double, double>> bids;
    std::vector<std::pair<double, double>> asks;
};

struct LiquidationEvent {
    std::string symbol;
    OrderSide side{OrderSide::Buy};
    OrderType type{OrderType::Limit};
    TimeInForce timeInForce{TimeInForce::GTC};
    std::string status;
    std::string price{"0"};
    std::string origQty{"0"};
    std::string lastFilledQty{"0"};
    std::string avgPrice{"0"};
    std::string cumFilledQty{"0"};
    int64_t time{0};
};

struct CompositeIndexEvent {
    std::string symbol;
    double price{0.0};
    int64_t time{0};
    struct Component {
        std::string baseAsset;
        std::string quoteAsset;
        double weightInQuantity{0.0};
        double weightInPercentage{0.0};
        double indexPrice{0.0};
    };
    std::vector<Component> components;
};

struct UnknownMarketEvent {
    std::string stream;
    std::string eventType;
};

using MarketEvent = std::variant<
    AggTradeEvent,
    KlineEvent,
    MarkPriceEvent,
    BookTickerEvent,
    DepthEvent,
    LiquidationEvent,
    CompositeIndexEvent,
    UnknownMarketEvent>;

struct OrderUpdateEvent {
    std::string symbol;
    std::string clientOrderId;
    std::string originalClientOrderId;
    int64_t orderId{0};
    OrderSide side{OrderSide::Buy};
    OrderType type{OrderType::Market};
    PositionSide positionSide{PositionSide::Both};
    TimeInForce timeInForce{TimeInForce::GTC};
    std::string executionType;
    std::string orderStatus;
    std::string originalQty{"0"};
    std::string originalPrice{"0"};
    std::string avgPrice{"0"};
    std::string lastFilledQty{"0"};
    std::string lastFilledPrice{"0"};
    std::string accumulatedFilledQty{"0"};
    std::string realizedPnl{"0"};
    std::string commission{"0"};
    std::string commissionAsset;
    bool isReduceOnly{false};
    bool closePosition{false};
    std::string stopPrice{"0"};
    std::string activationPrice{"0"};
    std::string priceRate{"0"};
    WorkingType workingType{WorkingType::ContractPrice};
    int64_t orderTime{0};
    int64_t tradeTime{0};
};

struct AccountUpdateEvent {
    std::string eventReason;
    std::vector<Balance> balances;
    std::vector<Position> positions;
    int64_t time{0};
};

struct MarginCallEvent {
    std::vector<Position> positions;
    int64_t time{0};
};

using UserDataEvent = std::variant<OrderUpdateEvent, AccountUpdateEvent, MarginCallEvent>;
