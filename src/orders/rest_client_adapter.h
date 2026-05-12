#pragma once

#include "orders/irest_client.h"
#include "rest/rest_client.h"

class RestClientAdapter final : public IRestClient {
public:
    explicit RestClientAdapter(RestClient& client) : m_client(client) {}

    boost::asio::awaitable<RestResult<Order>> newOrder(OrderRequest req) override;
    boost::asio::awaitable<RestResult<Order>> cancelOrder(std::string symbol, int64_t orderId) override;
    boost::asio::awaitable<RestResult<Order>> cancelOrderByClientOrderId(
        std::string symbol, std::string clientOrderId) override;
    boost::asio::awaitable<RestResult<void>> cancelAllOrders(std::string symbol) override;
    boost::asio::awaitable<RestResult<Order>> queryOrder(std::string symbol, int64_t orderId) override;
    boost::asio::awaitable<RestResult<Order>> queryOrderByClientOrderId(
        std::string symbol, std::string clientOrderId) override;
    boost::asio::awaitable<RestResult<std::vector<Order>>> openOrders(
        std::optional<std::string> symbol) override;
    boost::asio::awaitable<RestResult<std::vector<Order>>> allOrders(
        std::string symbol,
        std::optional<int64_t> startTime,
        std::optional<int64_t> endTime,
        int limit) override;
    boost::asio::awaitable<RestResult<BatchOrderResult>> batchOrders(std::vector<OrderRequest> reqs) override;

private:
    RestClient& m_client;
};
