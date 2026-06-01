#include "orders/order_validator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <string_view>

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

std::string upperSymbol(const std::string& symbol) {
    std::string out = symbol;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return out;
}

std::optional<long double> parseDecimal(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    try {
        const std::string owned(text);
        size_t parsed = 0;
        const long double value = std::stold(owned, &parsed);
        if (parsed != owned.size() || !std::isfinite(value)) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

bool isMultipleOfIncrement(long double value, long double increment) {
    if (increment <= 0.0L) {
        return true;
    }
    const long double scaled = value / increment;
    const long double nearest = std::round(scaled);
    const long double scaledError = std::abs(scaled - nearest);
    const long double absoluteError = std::abs(value - (nearest * increment));
    const long double tolerance = std::max(1e-12L, std::abs(increment) * 1e-9L);
    return scaledError <= 1e-9L || absoluteError <= tolerance;
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
    validateQuantityFilters(symbol, quantity, report);
}

void OrderValidator::validateQuantityFilters(const Symbol& symbol,
                                             const Quantity& quantity,
                                             ValidationReport& report) const {
    const auto* info = symbolInfo(symbol);
    if (info == nullptr) {
        return;
    }
    const auto parsed = parseDecimal(quantity.value());
    if (!parsed) {
        return;
    }

    const long double qty = *parsed;
    if (info->minQty > 0.0 && qty < static_cast<long double>(info->minQty)) {
        addIssue(report,
                 ValidationIssue::Severity::Error,
                 "quantity_below_min_qty",
                 "quantity is below exchange minQty for " + symbol);
    }
    if (info->maxQty > 0.0 && qty > static_cast<long double>(info->maxQty)) {
        addIssue(report,
                 ValidationIssue::Severity::Error,
                 "quantity_above_max_qty",
                 "quantity is above exchange maxQty for " + symbol);
    }
    if (info->stepSize > 0.0 && !isMultipleOfIncrement(qty, static_cast<long double>(info->stepSize))) {
        addIssue(report,
                 ValidationIssue::Severity::Error,
                 "quantity_step_size_mismatch",
                 "quantity must align with exchange stepSize for " + symbol);
    }
}

void OrderValidator::validatePriceFilters(const Symbol& symbol,
                                          const Price& price,
                                          std::string_view codePrefix,
                                          ValidationReport& report) const {
    const auto* info = symbolInfo(symbol);
    if (info == nullptr || info->tickSize <= 0.0) {
        return;
    }
    const auto parsed = parseDecimal(price.value());
    if (!parsed) {
        return;
    }
    if (!isMultipleOfIncrement(*parsed, static_cast<long double>(info->tickSize))) {
        addIssue(report,
                 ValidationIssue::Severity::Error,
                 std::string(codePrefix) + "_tick_size_mismatch",
                 "price must align with exchange tickSize for " + symbol);
    }
}

void OrderValidator::validateNotionalFilter(const Symbol& symbol,
                                            const Quantity& quantity,
                                            const Price& price,
                                            ValidationReport& report) const {
    const auto* info = symbolInfo(symbol);
    if (info == nullptr || info->minNotional <= 0.0) {
        return;
    }
    const auto qty = parseDecimal(quantity.value());
    const auto px = parseDecimal(price.value());
    if (!qty || !px) {
        return;
    }
    if ((*qty * *px) < static_cast<long double>(info->minNotional)) {
        addIssue(report,
                 ValidationIssue::Severity::Error,
                 "notional_below_min_notional",
                 "order notional is below exchange minNotional for " + symbol);
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
        if (k == "signature") {
            addIssue(report,
                     ValidationIssue::Severity::Error,
                     "raw_signature_blocked",
                     "raw signature is always blocked");
            continue;
        }
        if ((k == "recvWindow" || k == "timestamp") && !m_cfg.allowRawTimestampOverride) {
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
    report.exchangeInfoAge = exchangeInfoAge();
    const bool stale = exchangeInfoIsStale();
    if (m_cfg.exchangeInfoBySymbol.empty()) {
        addIssue(report,
                 m_cfg.requireExchangeInfo ? ValidationIssue::Severity::Error : ValidationIssue::Severity::Skipped,
                 "exchange_info_unavailable",
                 "exchange info snapshot not provided; exchange-rule checks skipped");
        (void)symbol;
        return;
    }
    if (stale) {
        addIssue(report,
                 m_cfg.requireExchangeInfo ? ValidationIssue::Severity::Error : ValidationIssue::Severity::Skipped,
                 "exchange_info_stale",
                 "exchange info snapshot is stale; exchange-rule checks skipped");
        (void)symbol;
        return;
    }
    if (symbolInfo(symbol) == nullptr) {
        addIssue(report,
                 m_cfg.requireExchangeInfo ? ValidationIssue::Severity::Error : ValidationIssue::Severity::Skipped,
                 "exchange_symbol_info_unavailable",
                 "exchange info snapshot has no filters for " + symbol);
    }
}

const ExchangeSymbol* OrderValidator::symbolInfo(const Symbol& symbol) const {
    if (symbol.empty() || exchangeInfoIsStale()) {
        return nullptr;
    }
    auto it = m_cfg.exchangeInfoBySymbol.find(symbol);
    if (it != m_cfg.exchangeInfoBySymbol.end()) {
        return &it->second;
    }
    const auto normalized = upperSymbol(symbol);
    it = m_cfg.exchangeInfoBySymbol.find(normalized);
    if (it != m_cfg.exchangeInfoBySymbol.end()) {
        return &it->second;
    }
    return nullptr;
}

bool OrderValidator::exchangeInfoIsStale() const {
    if (m_cfg.exchangeInfoBySymbol.empty() || m_cfg.exchangeInfoUpdatedAt == std::chrono::system_clock::time_point{} ||
        m_cfg.exchangeInfoMaxAge <= std::chrono::milliseconds::zero()) {
        return false;
    }
    const auto now = std::chrono::system_clock::now();
    if (now < m_cfg.exchangeInfoUpdatedAt) {
        return false;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - m_cfg.exchangeInfoUpdatedAt) >
           m_cfg.exchangeInfoMaxAge;
}

std::optional<std::chrono::milliseconds> OrderValidator::exchangeInfoAge() const {
    if (m_cfg.exchangeInfoUpdatedAt == std::chrono::system_clock::time_point{}) {
        return std::nullopt;
    }
    const auto now = std::chrono::system_clock::now();
    if (now < m_cfg.exchangeInfoUpdatedAt) {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - m_cfg.exchangeInfoUpdatedAt);
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
    } else {
        validatePriceFilters(draft.symbol, draft.price, "limit_price", report);
        validateNotionalFilter(draft.symbol, draft.quantity, draft.price, report);
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
    } else {
        validatePriceFilters(draft.symbol, draft.triggerPrice, "trigger_price", report);
        validateNotionalFilter(draft.symbol, draft.quantity, draft.triggerPrice, report);
    }

    if (draft.limitPrice) {
        if (draft.limitPrice->value().empty()) {
            addIssue(report, ValidationIssue::Severity::Error, "limit_price_empty", "limitPrice cannot be empty if provided");
        } else if (draft.limitPrice->toDouble() <= 0.0) {
            addIssue(report, ValidationIssue::Severity::Error, "limit_price_positive", "limitPrice must be > 0");
        } else {
            validatePriceFilters(draft.symbol, *draft.limitPrice, "limit_price", report);
            validateNotionalFilter(draft.symbol, draft.quantity, *draft.limitPrice, report);
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
    } else {
        validatePriceFilters(draft.symbol, draft.triggerPrice, "trigger_price", report);
        if (std::holds_alternative<Quantity>(draft.closeQuantity)) {
            validateNotionalFilter(draft.symbol, std::get<Quantity>(draft.closeQuantity), draft.triggerPrice, report);
        }
    }

    if (draft.positionSide == PositionSide::Long && draft.closeSide != OrderSide::Sell) {
        addIssue(
            report,
            ValidationIssue::Severity::Error,
            "protection_close_side_mismatch_long",
            "Long position protection must close with SELL side");
    } else if (draft.positionSide == PositionSide::Short && draft.closeSide != OrderSide::Buy) {
        addIssue(
            report,
            ValidationIssue::Severity::Error,
            "protection_close_side_mismatch_short",
            "Short position protection must close with BUY side");
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
