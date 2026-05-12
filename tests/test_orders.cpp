#include <gtest/gtest.h>

#include "orders/irest_client.h"
#include "orders/orders.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <stdexcept>

namespace {

class StubRestClient final : public IRestClient {
public:
    RestResult<Order> newOrderResult = std::unexpected(BinanceError::fromApiResponse(-1, "not set"));
    RestResult<Order> cancelOrderResult = std::unexpected(BinanceError::fromApiResponse(-1, "not set"));
    RestResult<Order> queryOrderResult = std::unexpected(BinanceError::fromApiResponse(-1, "not set"));

    boost::asio::awaitable<RestResult<Order>> newOrder(OrderRequest) override {
        co_return newOrderResult;
    }

    boost::asio::awaitable<RestResult<Order>> cancelOrder(std::string, int64_t) override {
        co_return cancelOrderResult;
    }

    boost::asio::awaitable<RestResult<Order>> cancelOrderByClientOrderId(std::string, std::string) override {
        co_return cancelOrderResult;
    }

    boost::asio::awaitable<RestResult<void>> cancelAllOrders(std::string) override {
        co_return RestResult<void>{};
    }

    boost::asio::awaitable<RestResult<Order>> queryOrder(std::string, int64_t) override {
        co_return queryOrderResult;
    }

    boost::asio::awaitable<RestResult<Order>> queryOrderByClientOrderId(std::string, std::string) override {
        co_return queryOrderResult;
    }

    boost::asio::awaitable<RestResult<std::vector<Order>>> openOrders(std::optional<std::string>) override {
        co_return std::vector<Order>{};
    }

    boost::asio::awaitable<RestResult<std::vector<Order>>> allOrders(std::string,
                                                                      std::optional<int64_t>,
                                                                      std::optional<int64_t>,
                                                                      int) override {
        co_return std::vector<Order>{};
    }

    boost::asio::awaitable<RestResult<BatchOrderResult>> batchOrders(std::vector<OrderRequest>) override {
        BatchOrderResult result;
        co_return result;
    }
};

template <typename T>
T runAwaitable(boost::asio::awaitable<T> task) {
    boost::asio::io_context ioc;
    auto fut = boost::asio::co_spawn(ioc, std::move(task), boost::asio::use_future);
    ioc.run();
    return fut.get();
}

Quantity qty(std::string_view v) {
    auto parsed = Quantity::parse(v);
    if (!parsed) {
        throw std::runtime_error("invalid test quantity");
    }
    return *parsed;
}

} // namespace

TEST(OrdersTest, MarketTreatsHttp500AsUnknownPendingReconcile) {
    StubRestClient rest;
    rest.newOrderResult = std::unexpected(BinanceError{
        .category = ErrorCategory::Api,
        .code = 500,
        .message = "internal",
    });

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.allowBestEffortJournal = true;
    Orders orders(rest, cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
        .clientOrderId = std::string("cid-1"),
    };

    auto result = runAwaitable(orders.market(draft));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->state, PlacementState::UnknownPendingReconcile);
    ASSERT_TRUE(result->errorCategory.has_value());
    EXPECT_EQ(*result->errorCategory, OrderErrorCategory::ExchangeReject);
    ASSERT_TRUE(result->binanceCode.has_value());
    EXPECT_EQ(*result->binanceCode, 500);
}

TEST(OrdersTest, QuerySnapshotPreservesDecimalStrings) {
    StubRestClient rest;
    Order order;
    order.symbol = "BTCUSDT";
    order.orderId = 12345;
    order.clientOrderId = "cid-1";
    order.side = OrderSide::Buy;
    order.type = OrderType::Limit;
    order.positionSide = PositionSide::Long;
    order.timeInForce = TimeInForce::GTC;
    order.status = "NEW";
    order.price = "12345.6700";
    order.origQty = "0.0500";
    order.executedQty = "0.0100";
    order.avgPrice = "12345.0000";
    order.cumQuote = "123.4500";
    order.reduceOnly = false;
    order.closePosition = false;
    order.stopPrice = "0.0000";
    order.workingType = WorkingType::ContractPrice;
    order.time = 1700000000000;
    order.updateTime = 1700000000001;
    rest.queryOrderResult = order;

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    Orders orders(rest, cfg);

    auto snapshotResult = runAwaitable(orders.queryNormalByOrderId("BTCUSDT", 12345));
    ASSERT_TRUE(snapshotResult.has_value());
    EXPECT_EQ(snapshotResult->price, "12345.6700");
    EXPECT_EQ(snapshotResult->origQty, "0.0500");
    EXPECT_EQ(snapshotResult->executedQty, "0.0100");
    EXPECT_EQ(snapshotResult->avgPrice, "12345.0000");
    EXPECT_EQ(snapshotResult->cumQuote, "123.4500");
    EXPECT_EQ(snapshotResult->stopPrice, "0.0000");
}

TEST(OrdersTest, CancelResultPreservesDecimalStrings) {
    StubRestClient rest;
    Order order;
    order.symbol = "ETHUSDT";
    order.orderId = 222;
    order.clientOrderId = "cid-2";
    order.side = OrderSide::Sell;
    order.type = OrderType::Limit;
    order.status = "CANCELED";
    order.origQty = "10.5000";
    order.executedQty = "2.1000";
    order.price = "2500.1200";
    rest.cancelOrderResult = order;

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    Orders orders(rest, cfg);

    auto cancelResult = runAwaitable(orders.cancelNormalByOrderId("ETHUSDT", 222));
    ASSERT_TRUE(cancelResult.has_value());
    EXPECT_EQ(cancelResult->origQty, "10.5000");
    EXPECT_EQ(cancelResult->executedQty, "2.1000");
    EXPECT_EQ(cancelResult->price, "2500.1200");
}
