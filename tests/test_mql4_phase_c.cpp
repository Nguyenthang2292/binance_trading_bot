#include <gtest/gtest.h>

#include "orders/irest_client.h"
#include "orders/orders.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

namespace {

class StubRestClient final : public IRestClient {
public:
    Order lastNewOrderRequest;
    RestResult<Order> newOrderResult = std::unexpected(BinanceError::fromApiResponse(-1, "not set"));

    boost::asio::awaitable<RestResult<Order>> newOrder(OrderRequest req) override {
        lastNewOrderRequest.symbol = req.symbol;
        lastNewOrderRequest.side = req.side;
        lastNewOrderRequest.type = req.type;
        lastNewOrderRequest.origQty = req.quantity;
        lastNewOrderRequest.price = req.price.value_or("");
        lastNewOrderRequest.stopPrice = req.stopPrice.value_or("");
        lastNewOrderRequest.closePosition = req.closePosition.value_or(false);
        lastNewOrderRequest.positionSide = req.positionSide;
        lastNewOrderRequest.clientOrderId = req.newClientOrderId.value_or("");
        co_return newOrderResult;
    }

    boost::asio::awaitable<RestResult<Order>> modifyOrder(OrderRequest) override { co_return newOrderResult; }
    boost::asio::awaitable<RestResult<Order>> cancelOrder(std::string, int64_t) override { co_return newOrderResult; }
    boost::asio::awaitable<RestResult<Order>> cancelOrderByClientOrderId(std::string, std::string) override { co_return newOrderResult; }
    boost::asio::awaitable<RestResult<void>> cancelAllOrders(std::string) override { co_return RestResult<void>{}; }
    boost::asio::awaitable<RestResult<Order>> queryOrder(std::string, int64_t) override { co_return newOrderResult; }
    boost::asio::awaitable<RestResult<Order>> queryOrderByClientOrderId(std::string, std::string) override { co_return newOrderResult; }
    boost::asio::awaitable<RestResult<std::vector<Order>>> openOrders(std::optional<std::string>) override { co_return std::vector<Order>{}; }
    boost::asio::awaitable<RestResult<std::vector<Order>>> allOrders(std::string, std::optional<int64_t>, std::optional<int64_t>, int) override { co_return std::vector<Order>{}; }
    boost::asio::awaitable<RestResult<std::vector<UserTrade>>> userTrades(
        std::string,
        std::optional<int64_t>,
        std::optional<int64_t>,
        std::optional<int64_t>,
        int) override {
        co_return std::vector<UserTrade>{};
    }

    boost::asio::awaitable<RestResult<BatchOrderResult>> batchOrders(std::vector<OrderRequest>) override { co_return BatchOrderResult{}; }
};

template <typename T>
T runAwaitable(boost::asio::awaitable<T> task) {
    boost::asio::io_context ioc;
    auto fut = boost::asio::co_spawn(ioc, std::move(task), boost::asio::use_future);
    ioc.run();
    return fut.get();
}

} // namespace

TEST(Mql4PhaseCTest, StopEntryMapsCorrectParams) {
    StubRestClient rest;
    Order placed;
    placed.symbol = "BTCUSDT";
    placed.orderId = 12345;
    placed.status = "NEW";
    rest.newOrderResult = placed;

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    Orders orders(rest, cfg);

    StopEntryDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = Quantity::parse("0.1").value(),
        .triggerPrice = TriggerPrice::parse("65000").value(),
    };

    auto result = runAwaitable(orders.stopEntry(std::move(draft)));

    if (!result) {
        FAIL() << "stopEntry failed: " << result.error().message;
    }
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(rest.lastNewOrderRequest.symbol, "BTCUSDT");
    EXPECT_EQ(rest.lastNewOrderRequest.type, OrderType::StopMarket);
    EXPECT_EQ(rest.lastNewOrderRequest.origQty, "0.1");
    EXPECT_EQ(rest.lastNewOrderRequest.stopPrice, "65000");
}

TEST(Mql4PhaseCTest, ProtectionMapsCorrectParams) {
    StubRestClient rest;
    Order placed;
    placed.symbol = "ETHUSDT";
    placed.orderId = 6789;
    placed.status = "NEW";
    rest.newOrderResult = placed;

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    Orders orders(rest, cfg);

    ProtectionOrderDraft draft{
        .symbol = "ETHUSDT",
        .positionSide = PositionSide::Long,
        .closeSide = OrderSide::Sell,
        .triggerPrice = TriggerPrice::parse("3000").value(),
        .closeQuantity = CloseEntirePosition{},
    };

    auto result = runAwaitable(orders.protection(std::move(draft)));

    if (!result) {
        FAIL() << "protection failed: " << result.error().message;
    }
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(rest.lastNewOrderRequest.symbol, "ETHUSDT");
    EXPECT_EQ(rest.lastNewOrderRequest.type, OrderType::StopMarket);
    EXPECT_EQ(rest.lastNewOrderRequest.closePosition, true);
    EXPECT_EQ(rest.lastNewOrderRequest.stopPrice, "3000");
    EXPECT_EQ(rest.lastNewOrderRequest.positionSide, PositionSide::Long);
}

TEST(Mql4PhaseCTest, StopEntryMapsAmbiguousTransportErrorToUnknownPendingReconcile) {
    StubRestClient rest;
    rest.newOrderResult = std::unexpected(BinanceError::fromNetwork(
        boost::asio::error::timed_out,
        NetworkErrorPhase::AfterSend));

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    Orders orders(rest, cfg);

    StopEntryDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = Quantity::parse("0.1").value(),
        .triggerPrice = TriggerPrice::parse("65000").value(),
    };

    auto result = runAwaitable(orders.stopEntry(std::move(draft)));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->state, PlacementState::UnknownPendingReconcile);
    ASSERT_TRUE(result->errorCategory.has_value());
    EXPECT_EQ(*result->errorCategory, OrderErrorCategory::Network);
}
