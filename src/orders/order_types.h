#pragma once

#include "orders/decimal_string.h"
#include "types/trade.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <variant>
#include <vector>

using Symbol = std::string;
using ClientOrderId = std::string;
using ClientAlgoId = std::string;
using CorrelationId = std::string;
using RawOrderParams = std::unordered_map<std::string, std::string>;

enum class ResponseType { ACK, RESULT };
enum class PositionMode { Unknown, OneWay, Hedge };
enum class PlacementState { Accepted, Rejected, UnknownPendingReconcile };
enum class OrderErrorCategory {
    Validation,
    Unsupported,
    ExchangeReject,
    RateLimit,
    Auth,
    Network,
    Timeout,
    CanceledBeforeSend,
    Parse,
    Journal,
    Unknown
};

struct ValidationIssue {
    enum class Severity { Error, Warning, Skipped };
    Severity severity{Severity::Error};
    std::string code;
    std::string message;
};

struct ValidationReport {
    std::vector<ValidationIssue> issues;
    std::optional<std::chrono::milliseconds> exchangeInfoAge;

    bool hasErrors() const {
        for (const auto& issue : issues) {
            if (issue.severity == ValidationIssue::Severity::Error) {
                return true;
            }
        }
        return false;
    }
};

struct MarketOrderDraft {
    Symbol symbol;
    OrderSide side{OrderSide::Buy};
    Quantity quantity;
    PositionSide positionSide{PositionSide::Both};
    std::optional<bool> reduceOnly;
    std::optional<ClientOrderId> clientOrderId;
    std::optional<ResponseType> responseType;
    RawOrderParams raw;
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
};

struct CloseByMarketDraft {
    Symbol symbol;
    OrderSide side;
    Quantity quantity;
    std::optional<ClientOrderId> clientOrderId;

    CloseByMarketDraft(Symbol symbolValue,
                       OrderSide sideValue,
                       Quantity quantityValue,
                       std::optional<ClientOrderId> clientOrderIdValue = std::nullopt)
        : symbol(std::move(symbolValue)),
          side(sideValue),
          quantity(std::move(quantityValue)),
          clientOrderId(std::move(clientOrderIdValue)) {}
};

using NormalOrderDraft = std::variant<MarketOrderDraft, LimitOrderDraft, CloseByMarketDraft>;

struct NormalPlacementResult {
    PlacementState state{PlacementState::Rejected};
    Symbol symbol;
    ClientOrderId clientOrderId;
    CorrelationId correlationId;
    std::optional<int64_t> orderId;
    std::optional<std::string> orderStatus;
    std::optional<OrderErrorCategory> errorCategory;
    std::optional<int> binanceCode;
    std::optional<std::string> binanceMessage;
    std::optional<int> httpStatus;
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

class OrderJournal;

struct OrdersConfig {
    std::string clientIdNamespace;
    bool allowBestEffortJournal{false};
    ResponseType defaultResponseType{ResponseType::ACK};
    std::chrono::milliseconds recvWindow{5000};
    bool allowRawTimestampOverride{false};
    PositionMode positionMode{PositionMode::Unknown};
    std::shared_ptr<OrderJournal> journal;
    bool journalIsDurable{false};
};
