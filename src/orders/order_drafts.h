#pragma once

#include "orders/decimal_string.h"
#include "orders/order_common.h"
#include "types/trade.h"

#include <optional>
#include <utility>
#include <variant>

struct MarketOrderDraft {
    Symbol symbol;
    OrderSide side{OrderSide::Buy};
    Quantity quantity;
    PositionSide positionSide{PositionSide::Both};
    std::optional<bool> reduceOnly;
    std::optional<ClientOrderId> clientOrderId;
    std::optional<ResponseType> responseType;
    RawOrderParams raw;
    std::optional<OrderMetadata> metadata;
};

struct LimitOrderDraft {
    Symbol symbol;
    OrderSide side{OrderSide::Buy};
    Quantity quantity;
    Price price;
    TimeInForce timeInForce{TimeInForce::GTC};
    PositionSide positionSide{PositionSide::Both};
    std::optional<bool> reduceOnly;
    std::optional<ClientOrderId> clientOrderId;
    std::optional<ResponseType> responseType;
    RawOrderParams raw;
    std::optional<OrderMetadata> metadata;
};

struct CloseByMarketDraft {
    Symbol symbol;
    OrderSide side;
    Quantity quantity;
    std::optional<ClientOrderId> clientOrderId;
    std::optional<OrderMetadata> metadata;

    CloseByMarketDraft(Symbol symbolValue,
                       OrderSide sideValue,
                       Quantity quantityValue,
                       std::optional<ClientOrderId> clientOrderIdValue = std::nullopt,
                       std::optional<OrderMetadata> metadataValue = std::nullopt)
        : symbol(std::move(symbolValue)),
          side(sideValue),
          quantity(std::move(quantityValue)),
          clientOrderId(std::move(clientOrderIdValue)),
          metadata(std::move(metadataValue)) {}
};

struct AmendLimitOrderDraft {
    NormalOrderIdentity identity;
    OrderSide side;
    Quantity quantity;
    Price price;
    std::optional<ResponseType> responseType;
    std::optional<int64_t> recvWindow;
};

struct StopEntryDraft {
    Symbol symbol;
    OrderSide side;
    Quantity quantity;
    TriggerPrice triggerPrice;
    std::optional<Price> limitPrice;
    WorkingType workingType{WorkingType::ContractPrice};
    std::optional<ClientAlgoId> clientAlgoId;
    std::optional<OrderMetadata> metadata;
};

struct CloseEntirePosition {};
enum class ProtectionKind { StopLoss, TakeProfit };

struct ProtectionOrderDraft {
    Symbol symbol;
    PositionSide positionSide;
    OrderSide closeSide;
    ProtectionKind kind{ProtectionKind::StopLoss};
    TriggerPrice triggerPrice;
    std::variant<Quantity, CloseEntirePosition> closeQuantity;
    std::optional<ClientAlgoId> clientAlgoId;
    std::optional<OrderMetadata> metadata;
};

using NormalOrderDraft = std::variant<MarketOrderDraft, LimitOrderDraft, CloseByMarketDraft>;
