#pragma once

#include "orders/order_types.h"

class OrderValidator {
public:
    explicit OrderValidator(const OrdersConfig& cfg) : m_cfg(cfg) {}

    ValidationReport validateMarket(const MarketOrderDraft& draft) const;
    ValidationReport validateLimit(const LimitOrderDraft& draft) const;
    ValidationReport validateCloseByMarket(const CloseByMarketDraft& draft) const;
    ValidationReport validateBatch(const std::vector<NormalOrderDraft>& drafts) const;

private:
    const OrdersConfig& m_cfg;

    void validateClientOrderId(const std::optional<ClientOrderId>& clientOrderId, ValidationReport& report) const;
    void validateCommon(const Symbol& symbol, const Quantity& quantity, ValidationReport& report) const;
    void validateRawParams(const RawOrderParams& raw, ValidationReport& report) const;
};
