#include <gtest/gtest.h>

#include "orders/irest_client.h"
#include "orders/orders.h"
#include "orders/mql4_adapter.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

namespace {

class StubRestClient final : public IRestClient {
public:
    Order lastNewOrderRequest;
    std::vector<Order> capturedNewOrderRequests;
    RestResult<Order> newOrderResult = compat::unexpected(BinanceError::fromApiResponse(-1, "not set"));

    boost::asio::awaitable<RestResult<Order>> newOrder(OrderRequest req) override {
        lastNewOrderRequest.symbol = req.symbol;
        lastNewOrderRequest.side = req.side;
        lastNewOrderRequest.type = req.type;
        lastNewOrderRequest.origQty = req.quantity;
        lastNewOrderRequest.price = req.price.value_or("");
        lastNewOrderRequest.stopPrice = req.stopPrice.value_or("");
        lastNewOrderRequest.positionSide = req.positionSide;
        capturedNewOrderRequests.push_back(lastNewOrderRequest);
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
    boost::asio::awaitable<RestResult<std::vector<UserTrade>>> userTrades(std::string, std::optional<int64_t>, std::optional<int64_t>, std::optional<int64_t>, int) override { co_return std::vector<UserTrade>{}; }
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

TEST(Mql4PhaseETest, OrderSendMapsBuyCorrectly) {
    StubRestClient rest;
    Order placed;
    placed.symbol = "BTCUSDT";
    placed.orderId = 12345;
    placed.status = "NEW";
    rest.newOrderResult = placed;

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.allowBestEffortJournal = true;
    Orders orders(rest, cfg);
    orders::mql4::Mql4Adapter adapter(orders);

    orders::mql4::MappedOrderSendDraft draft{
        .symbol = "BTCUSDT",
        .operation = orders::mql4::TradeOperation::Buy,
        .quantity = Quantity::parse("0.1").value(),
    };

    auto result = runAwaitable(adapter.orderSend(std::move(draft)));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(rest.lastNewOrderRequest.symbol, "BTCUSDT");
    EXPECT_EQ(rest.lastNewOrderRequest.side, OrderSide::Buy);
    EXPECT_EQ(rest.lastNewOrderRequest.type, OrderType::Market);
    EXPECT_EQ(rest.lastNewOrderRequest.origQty, "0.1");
}

TEST(Mql4PhaseETest, OrderSendMapsBuyLimitCorrectly) {
    StubRestClient rest;
    Order placed;
    placed.symbol = "ETHUSDT";
    placed.orderId = 6789;
    placed.status = "NEW";
    rest.newOrderResult = placed;

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.allowBestEffortJournal = true;
    Orders orders(rest, cfg);
    orders::mql4::Mql4Adapter adapter(orders);

    orders::mql4::MappedOrderSendDraft draft{
        .symbol = "ETHUSDT",
        .operation = orders::mql4::TradeOperation::BuyLimit,
        .quantity = Quantity::parse("0.5").value(),
        .price = Price::parse("3000").value(),
    };

    auto result = runAwaitable(adapter.orderSend(std::move(draft)));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(rest.lastNewOrderRequest.symbol, "ETHUSDT");
    EXPECT_EQ(rest.lastNewOrderRequest.side, OrderSide::Buy);
    EXPECT_EQ(rest.lastNewOrderRequest.type, OrderType::Limit);
    EXPECT_EQ(rest.lastNewOrderRequest.origQty, "0.5");
    EXPECT_EQ(rest.lastNewOrderRequest.price, "3000");
}

TEST(Mql4PhaseETest, OrderSendAttachesStopLossAndTakeProfitAsSeparateAlgoOrders) {
    StubRestClient rest;
    Order placed;
    placed.symbol = "BTCUSDT";
    placed.orderId = 12345;
    placed.status = "NEW";
    placed.executedQty = "0.1";
    rest.newOrderResult = placed;

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.allowBestEffortJournal = true;
    Orders orders(rest, cfg);
    orders::mql4::Mql4Adapter adapter(orders);

    orders::mql4::MappedOrderSendDraft draft{
        .symbol = "BTCUSDT",
        .operation = orders::mql4::TradeOperation::Buy,
        .quantity = Quantity::parse("0.1").value(),
        .stopLoss = TriggerPrice::parse("59000").value(),
        .takeProfit = TriggerPrice::parse("62000").value(),
    };

    auto result = runAwaitable(adapter.orderSend(std::move(draft)));

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(rest.capturedNewOrderRequests.size(), 3U);
    EXPECT_EQ(rest.capturedNewOrderRequests[0].type, OrderType::Market);
    EXPECT_EQ(rest.capturedNewOrderRequests[1].type, OrderType::StopMarket);
    EXPECT_EQ(rest.capturedNewOrderRequests[2].type, OrderType::TakeProfitMarket);
    EXPECT_EQ(rest.capturedNewOrderRequests[1].side, OrderSide::Sell);
    EXPECT_EQ(rest.capturedNewOrderRequests[2].side, OrderSide::Sell);
    EXPECT_EQ(rest.capturedNewOrderRequests[1].positionSide, PositionSide::Long);
    EXPECT_EQ(rest.capturedNewOrderRequests[2].positionSide, PositionSide::Long);
}

TEST(Mql4PhaseETest, OrderSendSkipsProtectionWhenExecutedQuantityIsNotConfirmed) {
    StubRestClient rest;
    Order placed;
    placed.symbol = "BTCUSDT";
    placed.orderId = 12345;
    placed.status = "NEW";
    placed.executedQty = "0";
    rest.newOrderResult = placed;

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.allowBestEffortJournal = true;
    Orders orders(rest, cfg);
    orders::mql4::Mql4Adapter adapter(orders);

    orders::mql4::MappedOrderSendDraft draft{
        .symbol = "BTCUSDT",
        .operation = orders::mql4::TradeOperation::Buy,
        .quantity = Quantity::parse("0.1").value(),
        .stopLoss = TriggerPrice::parse("59000").value(),
    };

    auto result = runAwaitable(adapter.orderSend(std::move(draft)));

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(rest.capturedNewOrderRequests.size(), 1U);
    ASSERT_FALSE(result->validation.issues.empty());
    bool foundProtectionSkipIssue = false;
    for (const auto& issue : result->validation.issues) {
        if (issue.code == "mql4_protection_qty_unconfirmed") {
            foundProtectionSkipIssue = true;
            break;
        }
    }
    EXPECT_TRUE(foundProtectionSkipIssue);
}

TEST(Mql4PhaseETest, OrderSendBuyStopCanMapOptionalLimitPriceToStopLimit) {
    StubRestClient rest;
    Order placed;
    placed.symbol = "BTCUSDT";
    placed.orderId = 12345;
    placed.status = "NEW";
    rest.newOrderResult = placed;

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.allowBestEffortJournal = true;
    Orders orders(rest, cfg);
    orders::mql4::Mql4Adapter adapter(orders);

    orders::mql4::MappedOrderSendDraft draft{
        .symbol = "BTCUSDT",
        .operation = orders::mql4::TradeOperation::BuyStop,
        .quantity = Quantity::parse("0.1").value(),
        .price = Price::parse("61000").value(),
        .limitPrice = Price::parse("61010").value(),
    };

    auto result = runAwaitable(adapter.orderSend(std::move(draft)));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(rest.lastNewOrderRequest.type, OrderType::Stop);
    EXPECT_EQ(rest.lastNewOrderRequest.stopPrice, "61000");
    EXPECT_EQ(rest.lastNewOrderRequest.price, "61010");
}
