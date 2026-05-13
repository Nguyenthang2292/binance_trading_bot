#include "orders/order_mapper.h"

#include <algorithm>

namespace {

void appendExtraParams(OrderRequest& req, const RawOrderParams& raw) {
    for (const auto& [k, v] : raw) {
        req.extraParams.emplace_back(k, v);
    }
}

} // namespace

std::string OrderMapper::responseTypeToString(ResponseType type) {
    return type == ResponseType::RESULT ? "RESULT" : "ACK";
}

std::optional<std::string> OrderMapper::rawGet(const RawOrderParams& raw, const std::string& key) {
    const auto it = raw.find(key);
    if (it == raw.end()) {
        return std::nullopt;
    }
    return it->second;
}

OrderRequest OrderMapper::toOrderRequest(const MarketOrderDraft& draft, const ClientOrderId& clientOrderId) const {
    OrderRequest req;
    req.symbol = draft.symbol;
    req.side = draft.side;
    req.type = OrderType::Market;
    req.positionSide = draft.positionSide;
    req.quantity = std::string(draft.quantity.value());
    req.reduceOnly = draft.reduceOnly;
    req.newClientOrderId = clientOrderId;
    const auto respType = draft.responseType.value_or(m_cfg.defaultResponseType);
    req.newOrderRespType = responseTypeToString(respType);
    if (!rawGet(draft.raw, "recvWindow")) {
        req.recvWindow = static_cast<int64_t>(m_cfg.recvWindow.count());
    }
    appendExtraParams(req, draft.raw);
    return req;
}

OrderRequest OrderMapper::toOrderRequest(const LimitOrderDraft& draft, const ClientOrderId& clientOrderId) const {
    OrderRequest req;
    req.symbol = draft.symbol;
    req.side = draft.side;
    req.type = OrderType::Limit;
    req.positionSide = draft.positionSide;
    req.quantity = std::string(draft.quantity.value());
    req.price = std::string(draft.price.value());
    req.timeInForce = draft.timeInForce;
    req.reduceOnly = draft.reduceOnly;
    req.newClientOrderId = clientOrderId;
    const auto respType = draft.responseType.value_or(m_cfg.defaultResponseType);
    req.newOrderRespType = responseTypeToString(respType);
    if (!rawGet(draft.raw, "recvWindow")) {
        req.recvWindow = static_cast<int64_t>(m_cfg.recvWindow.count());
    }
    appendExtraParams(req, draft.raw);
    return req;
}

OrderRequest OrderMapper::toOrderRequest(const CloseByMarketDraft& draft, const ClientOrderId& clientOrderId) const {
    OrderRequest req;
    req.symbol = draft.symbol;
    req.side = draft.side;
    req.type = OrderType::Market;
    req.positionSide = PositionSide::Both;
    req.quantity = std::string(draft.quantity.value());
    req.reduceOnly = true;
    req.newClientOrderId = clientOrderId;
    req.newOrderRespType = responseTypeToString(m_cfg.defaultResponseType);
    req.recvWindow = static_cast<int64_t>(m_cfg.recvWindow.count());
    return req;
}

OrderRequest OrderMapper::toOrderRequest(const AmendLimitOrderDraft& draft) const {
    OrderRequest req;
    req.symbol = draft.identity.symbol;
    req.side = draft.side;
    if (draft.identity.orderId) {
        req.orderId = *draft.identity.orderId;
    }
    if (draft.identity.clientOrderId) {
        req.origClientOrderId = draft.identity.clientOrderId;
    }
    req.quantity = std::string(draft.quantity.value());
    req.price = std::string(draft.price.value());
    const auto respType = draft.responseType.value_or(m_cfg.defaultResponseType);
    req.newOrderRespType = responseTypeToString(respType);
    req.recvWindow = draft.recvWindow.value_or(static_cast<int64_t>(m_cfg.recvWindow.count()));
    return req;
}

OrderRequest OrderMapper::toOrderRequest(const StopEntryDraft& draft, const ClientOrderId& clientOrderId) const {
    OrderRequest req;
    req.symbol = draft.symbol;
    req.side = draft.side;
    req.type = draft.limitPrice ? OrderType::Stop : OrderType::StopMarket;
    req.quantity = std::string(draft.quantity.value());
    req.stopPrice = std::string(draft.triggerPrice.value());
    if (draft.limitPrice) {
        req.price = std::string(draft.limitPrice->value());
        req.timeInForce = TimeInForce::GTC;
    }
    req.workingType = draft.workingType;
    req.newClientOrderId = clientOrderId;
    req.newOrderRespType = responseTypeToString(m_cfg.defaultResponseType);
    req.recvWindow = static_cast<int64_t>(m_cfg.recvWindow.count());
    return req;
}

OrderRequest OrderMapper::toOrderRequest(const ProtectionOrderDraft& draft, const ClientOrderId& clientOrderId) const {
    OrderRequest req;
    req.symbol = draft.symbol;
    req.side = draft.closeSide;
    req.type = draft.kind == ProtectionKind::TakeProfit
        ? OrderType::TakeProfitMarket
        : OrderType::StopMarket;
    req.positionSide = draft.positionSide;

    if (std::holds_alternative<Quantity>(draft.closeQuantity)) {
        req.quantity = std::string(std::get<Quantity>(draft.closeQuantity).value());
    } else {
        req.closePosition = true;
    }

    req.stopPrice = std::string(draft.triggerPrice.value());
    req.newClientOrderId = clientOrderId;
    req.newOrderRespType = responseTypeToString(m_cfg.defaultResponseType);
    req.recvWindow = static_cast<int64_t>(m_cfg.recvWindow.count());
    return req;
}
