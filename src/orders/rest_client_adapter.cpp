#include "orders/rest_client_adapter.h"

boost::asio::awaitable<RestResult<Order>> RestClientAdapter::newOrder(OrderRequest req) {
    co_return co_await m_client.newOrder(std::move(req));
}

boost::asio::awaitable<RestResult<Order>> RestClientAdapter::cancelOrder(std::string symbol, int64_t orderId) {
    co_return co_await m_client.cancelOrder(std::move(symbol), orderId);
}

boost::asio::awaitable<RestResult<Order>> RestClientAdapter::cancelOrderByClientOrderId(
    std::string symbol,
    std::string clientOrderId) {
    co_return co_await m_client.cancelOrderByClientOrderId(std::move(symbol), std::move(clientOrderId));
}

boost::asio::awaitable<RestResult<void>> RestClientAdapter::cancelAllOrders(std::string symbol) {
    co_return co_await m_client.cancelAllOrders(std::move(symbol));
}

boost::asio::awaitable<RestResult<Order>> RestClientAdapter::queryOrder(std::string symbol, int64_t orderId) {
    co_return co_await m_client.queryOrder(std::move(symbol), orderId);
}

boost::asio::awaitable<RestResult<Order>> RestClientAdapter::queryOrderByClientOrderId(
    std::string symbol,
    std::string clientOrderId) {
    co_return co_await m_client.queryOrderByClientOrderId(std::move(symbol), std::move(clientOrderId));
}

boost::asio::awaitable<RestResult<std::vector<Order>>> RestClientAdapter::openOrders(std::optional<std::string> symbol) {
    co_return co_await m_client.openOrders(std::move(symbol));
}

boost::asio::awaitable<RestResult<std::vector<Order>>> RestClientAdapter::allOrders(
    std::string symbol,
    std::optional<int64_t> startTime,
    std::optional<int64_t> endTime,
    int limit) {
    co_return co_await m_client.allOrders(std::move(symbol), startTime, endTime, limit);
}

boost::asio::awaitable<RestResult<BatchOrderResult>> RestClientAdapter::batchOrders(std::vector<OrderRequest> reqs) {
    co_return co_await m_client.batchOrders(std::move(reqs));
}
