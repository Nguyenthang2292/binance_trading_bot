#pragma once

#include "orders/orders.h"

namespace orders::mql4 {

enum class TradeOperation {
    Buy,
    Sell,
    BuyLimit,
    SellLimit,
    BuyStop,
    SellStop
};

struct MappedOrderSendDraft {
    Symbol symbol;
    TradeOperation operation;
    Quantity quantity;
    std::optional<Price> price;
    std::optional<Price> limitPrice;
    std::optional<TriggerPrice> stopLoss;
    std::optional<TriggerPrice> takeProfit;
    std::optional<OrderMetadata> metadata;
};

class Mql4Adapter {
public:
    explicit Mql4Adapter(Orders& orders) : m_orders(orders) {}

    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> orderSend(MappedOrderSendDraft draft);
    
    // Iteration helpers using snapshots
    boost::asio::awaitable<OrdersResult<OrderPoolSnapshot>> getOpenOrders(std::optional<Symbol> symbol = std::nullopt);

private:
    Orders& m_orders;
};

} // namespace orders::mql4
