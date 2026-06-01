#pragma once

#include "common/expected_compat.h"
#include "types/error.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

enum class OrderSide { Buy, Sell };
enum class OrderType {
    Limit,
    Market,
    Stop,
    StopMarket,
    TakeProfit,
    TakeProfitMarket,
    TrailingStopMarket
};
enum class TimeInForce { GTC, IOC, FOK, GTX };
enum class PositionSide { Both, Long, Short };
enum class WorkingType { MarkPrice, ContractPrice };

struct OrderRequest {
    std::string symbol;
    OrderSide side{OrderSide::Buy};
    OrderType type{OrderType::Market};
    PositionSide positionSide{PositionSide::Both};
    std::string quantity;
    std::optional<std::string> price;
    std::optional<std::string> stopPrice;
    std::optional<std::string> activationPrice;
    std::optional<std::string> callbackRate;
    std::optional<TimeInForce> timeInForce;
    std::optional<bool> reduceOnly;
    std::optional<bool> closePosition;
    std::optional<WorkingType> workingType;
    std::optional<std::string> newClientOrderId;
    std::optional<std::string> origClientOrderId;
    int64_t orderId{0};
    std::optional<std::string> newOrderRespType;
    std::optional<int64_t> recvWindow;
    std::vector<std::pair<std::string, std::string>> extraParams;
};

struct Order {
    std::string symbol;
    std::string clientOrderId;
    int64_t orderId{0};
    OrderSide side{OrderSide::Buy};
    OrderType type{OrderType::Market};
    PositionSide positionSide{PositionSide::Both};
    TimeInForce timeInForce{TimeInForce::GTC};
    std::string status;
    std::string price;
    std::string origQty;
    std::string executedQty;
    std::string avgPrice;
    std::string cumQuote;
    bool reduceOnly{false};
    bool closePosition{false};
    std::string stopPrice;
    std::string activationPrice;
    std::string priceRate;
    WorkingType workingType{WorkingType::ContractPrice};
    int64_t time{0};
    int64_t updateTime{0};
};

struct BatchOrderResult {
    std::vector<compat::expected<Order, BinanceError>> results;
};

struct UserTrade {
    std::string symbol;
    int64_t id{0};
    int64_t orderId{0};
    OrderSide side{OrderSide::Buy};
    std::string price;
    std::string qty;
    std::string realizedPnl;
    std::string marginAsset;
    std::string quoteQty;
    std::string commission;
    std::string commissionAsset;
    int64_t time{0};
    PositionSide positionSide{PositionSide::Both};
    bool maker{false};
    bool buyer{false};
};
