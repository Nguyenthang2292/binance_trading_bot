#include <gtest/gtest.h>

#include "orders/irest_client.h"
#include "orders/orders.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

namespace {

class StubRestClient final : public IRestClient {
public:
    Order lastModifyRequest;
    RestResult<Order> modifyResult = std::unexpected(BinanceError::fromApiResponse(-1, "not set"));

    boost::asio::awaitable<RestResult<Order>> newOrder(OrderRequest) override { co_return std::unexpected(BinanceError::fromApiResponse(-1, "not used")); }
    
    boost::asio::awaitable<RestResult<Order>> modifyOrder(OrderRequest req) override {
        lastModifyRequest.symbol = req.symbol;
        lastModifyRequest.side = req.side;
        lastModifyRequest.origQty = req.quantity;
        lastModifyRequest.price = req.price.value_or("");
        lastModifyRequest.orderId = req.orderId;
        lastModifyRequest.clientOrderId = req.origClientOrderId.value_or("");
        co_return modifyResult;
    }

    boost::asio::awaitable<RestResult<Order>> cancelOrder(std::string, int64_t) override { co_return modifyResult; }
    boost::asio::awaitable<RestResult<Order>> cancelOrderByClientOrderId(std::string, std::string) override { co_return modifyResult; }
    boost::asio::awaitable<RestResult<void>> cancelAllOrders(std::string) override { co_return RestResult<void>{}; }
    boost::asio::awaitable<RestResult<Order>> queryOrder(std::string, int64_t) override { co_return modifyResult; }
    boost::asio::awaitable<RestResult<Order>> queryOrderByClientOrderId(std::string, std::string) override { co_return modifyResult; }
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

TEST(Mql4PhaseBTest, AmendLimitOrderByOrderIdMapsCorrectParams) {
    StubRestClient rest;
    Order amended;
    amended.symbol = "BTCUSDT";
    amended.orderId = 12345;
    amended.status = "NEW";
    amended.price = "61000";
    amended.origQty = "0.02";
    rest.modifyResult = amended;

    OrdersConfig cfg;
    Orders orders(rest, cfg);

    auto result = runAwaitable(orders.amendLimitOrderByOrderId(
        "BTCUSDT", OrderSide::Buy, 12345, Quantity::parse("0.02").value(), Price::parse("61000").value()));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(rest.lastModifyRequest.symbol, "BTCUSDT");
    EXPECT_EQ(rest.lastModifyRequest.orderId, 12345);
    EXPECT_EQ(rest.lastModifyRequest.origQty, "0.02");
    EXPECT_EQ(rest.lastModifyRequest.price, "61000");
}

TEST(Mql4PhaseBTest, AmendLimitOrderByClientOrderIdMapsCorrectParams) {
    StubRestClient rest;
    Order amended;
    amended.symbol = "ETHUSDT";
    amended.clientOrderId = "my-cid-1";
    amended.status = "NEW";
    amended.price = "3100";
    amended.origQty = "0.5";
    rest.modifyResult = amended;

    OrdersConfig cfg;
    Orders orders(rest, cfg);

    auto result = runAwaitable(orders.amendLimitOrderByClientOrderId(
        "ETHUSDT", OrderSide::Sell, "my-cid-1", Quantity::parse("0.5").value(), Price::parse("3100").value()));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(rest.lastModifyRequest.symbol, "ETHUSDT");
    EXPECT_EQ(rest.lastModifyRequest.clientOrderId, "my-cid-1");
    EXPECT_EQ(rest.lastModifyRequest.origQty, "0.5");
    EXPECT_EQ(rest.lastModifyRequest.price, "3100");
}
