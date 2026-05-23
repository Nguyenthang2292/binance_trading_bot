#pragma once

#include "common/expected_compat.h"
#include "types/account.h"
#include "types/trade.h"
#include "types/error.h"

#include <boost/asio/awaitable.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

template <typename T>
using RestResult = std::expected<T, BinanceError>;

class IRestClient {
public:
    virtual ~IRestClient() = default;

    virtual boost::asio::awaitable<RestResult<Order>> newOrder(OrderRequest req) = 0;
    virtual boost::asio::awaitable<RestResult<Order>> newAlgoOrder(OrderRequest req) {
        co_return co_await newOrder(std::move(req));
    }
    virtual boost::asio::awaitable<RestResult<Order>> modifyOrder(OrderRequest req) = 0;
    virtual boost::asio::awaitable<RestResult<Order>> cancelOrder(std::string symbol, int64_t orderId) = 0;
    virtual boost::asio::awaitable<RestResult<Order>> cancelAlgoOrder(std::string symbol, int64_t algoId) {
        co_return co_await cancelOrder(std::move(symbol), algoId);
    }
    virtual boost::asio::awaitable<RestResult<Order>> cancelOrderByClientOrderId(
        std::string symbol, std::string clientOrderId) = 0;
    virtual boost::asio::awaitable<RestResult<Order>> cancelAlgoOrderByClientAlgoId(
        std::string symbol, std::string clientAlgoId) {
        co_return co_await cancelOrderByClientOrderId(std::move(symbol), std::move(clientAlgoId));
    }
    virtual boost::asio::awaitable<RestResult<void>> cancelAllOrders(std::string symbol) = 0;
    virtual boost::asio::awaitable<RestResult<Order>> queryOrder(std::string symbol, int64_t orderId) = 0;
    virtual boost::asio::awaitable<RestResult<Order>> queryAlgoOrder(std::string symbol, int64_t algoId) {
        co_return co_await queryOrder(std::move(symbol), algoId);
    }
    virtual boost::asio::awaitable<RestResult<Order>> queryOrderByClientOrderId(
        std::string symbol, std::string clientOrderId) = 0;
    virtual boost::asio::awaitable<RestResult<Order>> queryAlgoOrderByClientAlgoId(
        std::string symbol, std::string clientAlgoId) {
        co_return co_await queryOrderByClientOrderId(std::move(symbol), std::move(clientAlgoId));
    }
    virtual boost::asio::awaitable<RestResult<std::vector<Order>>> openOrders(
        std::optional<std::string> symbol = {}) = 0;
    virtual boost::asio::awaitable<RestResult<std::vector<Order>>> allOrders(
        std::string symbol,
        std::optional<int64_t> startTime = {},
        std::optional<int64_t> endTime = {},
        int limit = 500) = 0;
    virtual boost::asio::awaitable<RestResult<std::vector<UserTrade>>> userTrades(
        std::string symbol,
        std::optional<int64_t> orderId = {},
        std::optional<int64_t> startTime = {},
        std::optional<int64_t> endTime = {},
        int limit = 500) = 0;
    virtual boost::asio::awaitable<RestResult<LeverageResult>> setLeverage(std::string, int) {
        co_return std::unexpected(BinanceError::fromApiResponse(-1, "setLeverage unsupported"));
    }
    virtual boost::asio::awaitable<RestResult<BatchOrderResult>> batchOrders(std::vector<OrderRequest> reqs) = 0;
};
