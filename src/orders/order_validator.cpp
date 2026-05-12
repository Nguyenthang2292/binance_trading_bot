#include "orders/order_validator.h"

#include <regex>
#include <type_traits>
#include <unordered_set>

namespace {

constexpr size_t kMaxBatchOrders = 5;
const std::regex kClientOrderIdPattern("^[A-Za-z0-9._:@/-]{1,36}$");

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

} // namespace

void OrderValidator::validateClientOrderId(const std::optional<ClientOrderId>& clientOrderId,
                                           ValidationReport& report) const {
    if (!clientOrderId.has_value()) {
        return;
    }
    if (!std::regex_match(*clientOrderId, kClientOrderIdPattern)) {
        addIssue(report,
                 ValidationIssue::Severity::Error,
                 "client_order_id_invalid",
                 "clientOrderId must match [A-Za-z0-9._:@/-] and be length 1..36");
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
    if (raw.empty()) {
        return;
    }

    const std::unordered_set<std::string> blockedKeys = {
        "symbol", "side", "type", "quantity", "price", "positionSide",
        "newClientOrderId", "timestamp", "signature",
    };

    for (const auto& [k, _] : raw) {
        if (blockedKeys.contains(k)) {
            addIssue(report,
                     ValidationIssue::Severity::Error,
                     "raw_param_blocked",
                     "raw param key '" + k + "' is blocked");
            continue;
        }

        if ((k == "recvWindow") && !m_cfg.allowRawTimestampOverride) {
            addIssue(report,
                     ValidationIssue::Severity::Error,
                     "raw_recvwindow_blocked",
                     "raw recvWindow is blocked by configuration");
        }
    }
}

ValidationReport OrderValidator::validateMarket(const MarketOrderDraft& draft) const {
    ValidationReport report;
    validateCommon(draft.symbol, draft.quantity, report);
    validateClientOrderId(draft.clientOrderId, report);
    validateRawParams(draft.raw, report);

    if (draft.reduceOnly.value_or(false) && m_cfg.positionMode == PositionMode::Hedge) {
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

    if (draft.price.value().empty() || draft.price.toDouble() <= 0.0) {
        addIssue(report, ValidationIssue::Severity::Error, "price_positive", "limit price must be > 0");
    }

    if (draft.reduceOnly.value_or(false) && m_cfg.positionMode == PositionMode::Hedge) {
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

    if (m_cfg.positionMode != PositionMode::OneWay) {
        addIssue(report,
                 ValidationIssue::Severity::Error,
                 "close_by_market_requires_one_way",
                 "closeByMarket requires confirmed OneWay mode");
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
