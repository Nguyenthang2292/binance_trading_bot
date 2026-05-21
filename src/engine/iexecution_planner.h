#pragma once

#include "orders/orders.h"
#include <boost/asio/awaitable.hpp>
#include <string>

namespace engine {

struct ExecutionPlan {
    std::string planId;
    bool isAsync{false};
};

class IExecutionPlanner {
public:
    virtual ~IExecutionPlanner() = default;
    
    virtual boost::asio::awaitable<OrdersResult<NormalPlacementResult>> executeMarket(MarketOrderDraft draft) = 0;
};

class IOrdersPort;

class NativeExecutionPlanner : public IExecutionPlanner {
public:
    explicit NativeExecutionPlanner(IOrdersPort& ordersPort);
    ~NativeExecutionPlanner() override = default;
    
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> executeMarket(MarketOrderDraft draft) override;

private:
    IOrdersPort& m_orders;
};

} // namespace engine
