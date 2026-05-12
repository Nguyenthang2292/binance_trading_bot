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
    req.recvWindow = static_cast<int64_t>(m_cfg.recvWindow.count());
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
    req.recvWindow = static_cast<int64_t>(m_cfg.recvWindow.count());
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
