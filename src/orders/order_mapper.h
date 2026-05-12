#pragma once

#include "orders/order_types.h"

class OrderMapper {
public:
    explicit OrderMapper(const OrdersConfig& cfg) : m_cfg(cfg) {}

    OrderRequest toOrderRequest(const MarketOrderDraft& draft, const ClientOrderId& clientOrderId) const;
    OrderRequest toOrderRequest(const LimitOrderDraft& draft, const ClientOrderId& clientOrderId) const;
    OrderRequest toOrderRequest(const CloseByMarketDraft& draft, const ClientOrderId& clientOrderId) const;

private:
    const OrdersConfig& m_cfg;

    static std::string responseTypeToString(ResponseType type);
    static std::optional<std::string> rawGet(const RawOrderParams& raw, const std::string& key);
};
