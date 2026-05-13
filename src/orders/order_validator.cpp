#include "orders/order_validator.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace {

void addIssue(ValidationReport& report,
              ValidationIssue::Severity severity,
              std::string code,
              std::string message) {
    report.issues.push_back(ValidationIssue{
        .severity = severity,
        .code = std::move(code),
        .message = std::move(message),
    });
}

constexpr size_t kMaxBatchOrders = 5;

bool isConservativeRawKey(const std::string& key) {
    if (key.empty() || key.size() > 64) {
        return false;
    }

    const unsigned char first = static_cast<unsigned char>(key.front());
    if (!std::isalpha(first)) {
        return false;
    }

    return std::all_of(key.begin() + 1, key.end(), [](char c) {
        const unsigned char uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) || c == '_';
    });
}

bool isAlwaysBlockedRawKey(const std::string& key) {
    static const std::array<std::string_view, 11> blocked{
        "symbol",
        "side",
        "type",
        "quantity",
        "price",
        "positionSide",
        "newClientOrderId",
        "newOrderRespType",
        "timeInForce",
        "reduceOnly",
        "clientAlgoId",
    };
    return std::find(blocked.begin(), blocked.end(), key) != blocked.end();
}

} // namespace

void OrderValidator::validateClientOrderId(const std::optional<ClientOrderId>& clientOrderId,
                                           ValidationReport& report) const {
    if (!clientOrderId) {
        return;
    }
    if (clientOrderId->size() > 36) {
        addIssue(report,
                 ValidationIssue::Severity::Error,
                 "client_order_id_too_long",
                 "clientOrderId max length is 36");
    }
}

void OrderValidator::validateCommon(const Symbol& symbol, const Quantity& quantity, ValidationReport& report) const {
    if (symbol.empty()) {
        addIssue(report, ValidationIssue::Severity::Error, "symbol_required", "symbol is required");
    }
    if (quantity.value().empty()) {
        addIssue(report, ValidationIssue::Severity::Error, "quantity_required", "quantity is required");
    } else if (quantity.toDouble() <= 0.0) {
        addIssue(report, ValidationIssue::Severity::Error, "quantity_positive", "quantity must be > 0");
    }
}

void OrderValidator::validateRawParams(const RawOrderParams& raw, ValidationReport& report) const {
    for (const auto& [k, v] : raw) {
        (void)v;
        if (k.empty()) {
            addIssue(report, ValidationIssue::Severity::Error, "raw_param_key_empty", "raw parameter key cannot be empty");
            continue;
        }
        if (!isConservativeRawKey(k)) {
            addIssue(report,
                     ValidationIssue::Severity::Error,
                     "raw_param_key_invalid",
                     "raw parameter key must match ^[A-Za-z][A-Za-z0-9_]{0,63}$");
            continue;
        }
        if (isAlwaysBlockedRawKey(k)) {
            addIssue(report,
                     ValidationIssue::Severity::Error,
                     "raw_param_blocked",
                     "raw parameter key is blocked: " + k);
            continue;
        }
        if ((k == "recvWindow" || k == "timestamp" || k == "signature") && !m_cfg.allowRawTimestampOverride) {
            addIssue(report,
                     ValidationIssue::Severity::Error,
                     "raw_recvwindow_blocked",
                     "raw " + k + " is blocked by configuration");
        }
    }
}

void OrderValidator::addAdvisoryIssues(const Symbol& symbol, ValidationReport& report) const {
    if (m_cfg.positionMode == PositionMode::Unknown) {
        addIssue(report,
                 ValidationIssue::Severity::Warning,
                 "position_mode_unknown",
                 "position mode is unknown; reduceOnly/position-side checks may be incomplete");
    }
    if (m_cfg.clientIdNamespace.empty()) {
        addIssue(report,
                 ValidationIssue::Severity::Warning,
                 "no_client_id_namespace",
                 "no clientIdNamespace configured; using default prefix");
    }
    addIssue(report,
             ValidationIssue::Severity::Skipped,
             "exchange_info_unavailable",
             "exchange info snapshot not provided; exchange-rule checks skipped");
    (void)symbol;
}

ValidationReport OrderValidator::validateMarket(const MarketOrderDraft& draft) const {
    ValidationReport report;
    validateCommon(draft.symbol, draft.quantity, report);
    validateClientOrderId(draft.clientOrderId, report);
    validateRawParams(draft.raw, report);
    addAdvisoryIssues(draft.symbol, report);
    if (draft.reduceOnly && *draft.reduceOnly && m_cfg.positionMode == PositionMode::Hedge) {
        addIssue(report,
                 ValidationIssue::Severity::Error,
                 "reduce_only_hedge_forbidden",
                 "reduceOnly cannot be sent in Hedge Mode");
    }
    return report;
}

ValidationReport OrderValidator::validateLimit(const LimitOrderDraft& draft) const {
    ValidationReport report;
    validateCommon(draft.symbol, draft.quantity, report);
    validateClientOrderId(draft.clientOrderId, report);
    validateRawParams(draft.raw, report);
    addAdvisoryIssues(draft.symbol, report);

    if (draft.price.value().empty()) {
        addIssue(report,
                 ValidationIssue::Severity::Error,
                 "limit_price_required",
                 "price is required for LIMIT order");
    } else if (draft.price.toDouble() <= 0.0) {
        addIssue(report,
                 ValidationIssue::Severity::Error,
                 "limit_price_positive",
                 "limit price must be > 0");
    }

    if (draft.reduceOnly && *draft.reduceOnly && m_cfg.positionMode == PositionMode::Hedge) {
        addIssue(report,
                 ValidationIssue::Severity::Error,
                 "reduce_only_hedge_forbidden",
                 "reduceOnly cannot be sent in Hedge Mode");
    }
    return report;
}

ValidationReport OrderValidator::validateCloseByMarket(const CloseByMarketDraft& draft) const {
    ValidationReport report;
    validateCommon(draft.symbol, draft.quantity, report);
    validateClientOrderId(draft.clientOrderId, report);
    addAdvisoryIssues(draft.symbol, report);

    if (m_cfg.positionMode != PositionMode::OneWay) {
        addIssue(report,
                 ValidationIssue::Severity::Error,
                 "close_by_market_requires_one_way",
                 "closeByMarket requires confirmed OneWay mode");
    }
    return report;
}

ValidationReport OrderValidator::validateStopEntry(const StopEntryDraft& draft) const {
    ValidationReport report;
    validateCommon(draft.symbol, draft.quantity, report);
    validateClientOrderId(draft.clientAlgoId, report);
    addAdvisoryIssues(draft.symbol, report);

    if (draft.triggerPrice.value().empty()) {
        addIssue(report, ValidationIssue::Severity::Error, "trigger_price_required", "triggerPrice is required");
    } else if (draft.triggerPrice.toDouble() <= 0.0) {
        addIssue(report, ValidationIssue::Severity::Error, "trigger_price_positive", "triggerPrice must be > 0");
    }

    if (draft.limitPrice) {
        if (draft.limitPrice->value().empty()) {
            addIssue(report, ValidationIssue::Severity::Error, "limit_price_empty", "limitPrice cannot be empty if provided");
        } else if (draft.limitPrice->toDouble() <= 0.0) {
            addIssue(report, ValidationIssue::Severity::Error, "limit_price_positive", "limitPrice must be > 0");
        }
    }

    return report;
}

ValidationReport OrderValidator::validateProtection(const ProtectionOrderDraft& draft) const {
    ValidationReport report;
    if (std::holds_alternative<Quantity>(draft.closeQuantity)) {
        validateCommon(draft.symbol, std::get<Quantity>(draft.closeQuantity), report);
    } else {
        if (draft.symbol.empty()) {
            addIssue(report, ValidationIssue::Severity::Error, "symbol_required", "symbol is required");
        }
    }
    validateClientOrderId(draft.clientAlgoId, report);
    addAdvisoryIssues(draft.symbol, report);

    if (draft.triggerPrice.value().empty()) {
        addIssue(report, ValidationIssue::Severity::Error, "trigger_price_required", "triggerPrice is required");
    } else if (draft.triggerPrice.toDouble() <= 0.0) {
        addIssue(report, ValidationIssue::Severity::Error, "trigger_price_positive", "triggerPrice must be > 0");
    }

    return report;
}

ValidationReport OrderValidator::validateBatch(const std::vector<NormalOrderDraft>& drafts) const {
    ValidationReport report;
    if (drafts.empty()) {
        addIssue(report, ValidationIssue::Severity::Error, "batch_empty", "batch drafts cannot be empty");
        return report;
    }
    if (drafts.size() > kMaxBatchOrders) {
        addIssue(report, ValidationIssue::Severity::Error, "batch_too_large", "batch max size is 5");
    }

    for (const auto& draft : drafts) {
        ValidationReport itemReport = std::visit(
            [this](const auto& item) -> ValidationReport {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, MarketOrderDraft>) {
                    return validateMarket(item);
                } else if constexpr (std::is_same_v<T, LimitOrderDraft>) {
                    return validateLimit(item);
                } else {
                    return validateCloseByMarket(item);
                }
            },
            draft);
        report.issues.insert(report.issues.end(), itemReport.issues.begin(), itemReport.issues.end());
    }
    return report;
}
