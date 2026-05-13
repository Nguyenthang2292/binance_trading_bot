#pragma once

#include "orders/order_types.h"

#include <optional>
#include <string>

namespace orders::detail {

inline void addValidationIssue(ValidationReport& report, std::string code, std::string message) {
    report.issues.push_back(ValidationIssue{
        .severity = ValidationIssue::Severity::Error,
        .code = std::move(code),
        .message = std::move(message),
    });
}

inline std::string typeToString(OrderType type) {
    switch (type) {
        case OrderType::Limit: return "LIMIT";
        case OrderType::Market: return "MARKET";
        case OrderType::Stop: return "STOP";
        case OrderType::StopMarket: return "STOP_MARKET";
        case OrderType::TakeProfit: return "TAKE_PROFIT";
        case OrderType::TakeProfitMarket: return "TAKE_PROFIT_MARKET";
        case OrderType::TrailingStopMarket: return "TRAILING_STOP_MARKET";
    }
    return "MARKET";
}

inline std::string stateToString(PlacementState state) {
    switch (state) {
        case PlacementState::Accepted: return "Accepted";
        case PlacementState::Rejected: return "Rejected";
        case PlacementState::UnknownPendingReconcile: return "UnknownPendingReconcile";
    }
    return "Unknown";
}

inline std::string errorCategoryToString(std::optional<OrderErrorCategory> category) {
    if (!category) {
        return "none";
    }
    switch (*category) {
        case OrderErrorCategory::Validation: return "Validation";
        case OrderErrorCategory::Unsupported: return "Unsupported";
        case OrderErrorCategory::ExchangeReject: return "ExchangeReject";
        case OrderErrorCategory::RateLimit: return "RateLimit";
        case OrderErrorCategory::Auth: return "Auth";
        case OrderErrorCategory::Network: return "Network";
        case OrderErrorCategory::Timeout: return "Timeout";
        case OrderErrorCategory::CanceledBeforeSend: return "CanceledBeforeSend";
        case OrderErrorCategory::Parse: return "Parse";
        case OrderErrorCategory::Journal: return "Journal";
        case OrderErrorCategory::Unknown: return "Unknown";
    }
    return "Unknown";
}

inline std::optional<int> httpStatusFromError(const BinanceError& error) {
    if (error.code >= 100 && error.code < 600) {
        return error.code;
    }
    return std::nullopt;
}

inline void attachErrorDetails(NormalPlacementResult& result, const BinanceError& error) {
    result.binanceCode = error.code == 0 ? std::nullopt : std::optional<int>{error.code};
    result.binanceMessage = error.message;
    result.httpStatus = httpStatusFromError(error);
    result.rawResponseBody = error.message;
}

inline OrderErrorCategory mapErrorCategory(const BinanceError& error) {
    if (error.code == -1007 || error.code == -1008) {
        return OrderErrorCategory::Timeout;
    }
    switch (error.category) {
        case ErrorCategory::Auth: return OrderErrorCategory::Auth;
        case ErrorCategory::RateLimit: return OrderErrorCategory::RateLimit;
        case ErrorCategory::Network:
            if (error.isOperationAbortedBeforeSend()) {
                return OrderErrorCategory::CanceledBeforeSend;
            }
            return OrderErrorCategory::Network;
        case ErrorCategory::Parse: return OrderErrorCategory::Parse;
        case ErrorCategory::Api: return OrderErrorCategory::ExchangeReject;
    }
    return OrderErrorCategory::Unknown;
}

inline bool isAmbiguousPlacementError(const BinanceError& error) {
    if (error.isOperationAbortedBeforeSend()) {
        return false;
    }
    if (error.category == ErrorCategory::Network || error.category == ErrorCategory::Parse) {
        return true;
    }
    if (error.category == ErrorCategory::Api) {
        if (error.code == -1007 || error.code == -1008) {
            return true;
        }
        if (error.code >= 500 && error.code < 600) {
            return true;
        }
    }
    return false;
}

} // namespace orders::detail
