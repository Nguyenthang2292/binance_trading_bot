#include "engine/iexecution_planner.h"
#include "engine/signal_engine.h"

namespace engine {

NativeExecutionPlanner::NativeExecutionPlanner(IOrdersPort& ordersPort) : m_orders(ordersPort) {}

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> NativeExecutionPlanner::executeMarket(MarketOrderDraft draft) {
    co_return co_await m_orders.market(std::move(draft));
}

} // namespace engine
