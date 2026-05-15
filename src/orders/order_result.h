#pragma once

#include "orders/order_common.h"
#include "types/trade.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <variant>
#include <span>

struct NormalPlacementResult {
    PlacementState state{PlacementState::Rejected};
    Symbol symbol;
    ClientOrderId clientOrderId;
    CorrelationId correlationId;
    std::optional<int64_t> orderId;
    std::optional<std::string> orderStatus;
    std::optional<std::string> avgPrice;
    std::optional<OrderErrorCategory> errorCategory;
    std::optional<int> binanceCode;
    std::optional<std::string> binanceMessage;
    std::optional<int> httpStatus;
    std::optional<std::string> endpoint;
    std::optional<std::string> rawResponseBody;
    ValidationReport validation;
};

struct NormalCancelResult {
    Symbol symbol;
    int64_t orderId{0};
    ClientOrderId clientOrderId;
    std::string orderStatus;
    std::string side;
    std::string type;
    std::string origQty;
    std::string executedQty;
    std::string price;
};

struct NormalOrderSnapshot {
    Symbol symbol;
    int64_t orderId{0};
    ClientOrderId clientOrderId;
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
    WorkingType workingType{WorkingType::ContractPrice};
    int64_t time{0};
    int64_t updateTime{0};
};

struct BatchPlacementResult {
    std::vector<NormalPlacementResult> items;
};

enum class FillSummaryCompleteness {
    Unavailable,
    Partial,
    Complete
};

struct OrderFillSummary {
    Symbol symbol;
    int64_t orderId{0};
    FillSummaryCompleteness completeness{FillSummaryCompleteness::Unavailable};
    std::string executedQty;
    std::optional<std::string> avgEntryPrice;
    std::optional<std::string> avgExitPrice;
    std::optional<std::string> realizedPnl;
    std::optional<std::string> commission;
    std::optional<std::string> commissionAsset;
    std::optional<int64_t> firstTradeTime;
    std::optional<int64_t> lastTradeTime;
};

struct OrderView {
    OrderIdentity identity;
    NormalOrderSnapshot normal;
    std::optional<OrderMetadata> metadata;
    std::optional<OrderFillSummary> fills;
};

struct OrderPoolSnapshot {
    OrderPoolKind kind;
    std::chrono::system_clock::time_point capturedAt;
    std::vector<OrderView> orders;

    size_t count() const { return orders.size(); }
    std::span<const OrderView> items() const { return orders; }
    std::optional<OrderView> atSnapshotIndex(size_t index) const;
    std::optional<OrderView> byIdentity(const OrderIdentity& identity) const;
};

std::string formatOrderView(const OrderView& view, bool includeMetadata = false);
