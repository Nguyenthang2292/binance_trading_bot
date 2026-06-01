#include <gtest/gtest.h>

#include "orders/irest_client.h"
#include "orders/orders.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

namespace {

class StubRestClient final : public IRestClient {
public:
    std::vector<UserTrade> userTradesResult;

    boost::asio::awaitable<RestResult<Order>> newOrder(OrderRequest) override { co_return compat::unexpected(BinanceError::fromApiResponse(-1, "not used")); }
    boost::asio::awaitable<RestResult<Order>> modifyOrder(OrderRequest) override { co_return compat::unexpected(BinanceError::fromApiResponse(-1, "not used")); }
    boost::asio::awaitable<RestResult<Order>> cancelOrder(std::string, int64_t) override { co_return compat::unexpected(BinanceError::fromApiResponse(-1, "not used")); }
    boost::asio::awaitable<RestResult<Order>> cancelOrderByClientOrderId(std::string, std::string) override { co_return compat::unexpected(BinanceError::fromApiResponse(-1, "not used")); }
    boost::asio::awaitable<RestResult<void>> cancelAllOrders(std::string) override { co_return RestResult<void>{}; }
    boost::asio::awaitable<RestResult<Order>> queryOrder(std::string, int64_t) override { co_return compat::unexpected(BinanceError::fromApiResponse(-1, "not used")); }
    boost::asio::awaitable<RestResult<Order>> queryOrderByClientOrderId(std::string, std::string) override { co_return compat::unexpected(BinanceError::fromApiResponse(-1, "not used")); }
    boost::asio::awaitable<RestResult<std::vector<Order>>> openOrders(std::optional<std::string>) override { co_return std::vector<Order>{}; }
    boost::asio::awaitable<RestResult<std::vector<Order>>> allOrders(std::string, std::optional<int64_t>, std::optional<int64_t>, int) override { co_return std::vector<Order>{}; }
    
    boost::asio::awaitable<RestResult<std::vector<UserTrade>>> userTrades(std::string, std::optional<int64_t>, std::optional<int64_t>, std::optional<int64_t>, int) override {
        co_return userTradesResult;
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

TEST(Mql4PhaseDTest, QueryOrderFillSummaryAggregatesTrades) {
    StubRestClient rest;
    
    UserTrade t1;
    t1.symbol = "BTCUSDT";
    t1.orderId = 12345;
    t1.qty = "0.01";
    t1.price = "60000";
    t1.realizedPnl = "0";
    t1.commission = "1.2";
    t1.commissionAsset = "USDT";
    t1.time = 1000;

    UserTrade t2;
    t2.symbol = "BTCUSDT";
    t2.orderId = 12345;
    t2.qty = "0.02";
    t2.price = "61000";
    t2.realizedPnl = "5.0";
    t2.commission = "2.4";
    t2.commissionAsset = "USDT";
    t2.time = 2000;

    rest.userTradesResult = {t1, t2};

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    Orders orders(rest, cfg);

    auto result = runAwaitable(orders.queryOrderFillSummary("BTCUSDT", 12345));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->orderId, 12345);
    EXPECT_EQ(result->completeness, FillSummaryCompleteness::Complete);
    
    double execQty = std::stod(result->executedQty);
    EXPECT_NEAR(execQty, 0.03, 0.0001);
    
    // This order realized PnL (t2 rp=5.0), so it reduced a position: the VWAP is
    // an exit price, reported in avgExitPrice — never duplicated into entry.
    EXPECT_FALSE(result->avgEntryPrice.has_value());
    ASSERT_TRUE(result->avgExitPrice.has_value());
    double avgPrice = std::stod(*result->avgExitPrice);
    EXPECT_NEAR(avgPrice, 60666.66666, 0.1);

    EXPECT_EQ(result->commissionAsset, "USDT");
    double totalComm = std::stod(*result->commission);
    EXPECT_NEAR(totalComm, 3.6, 0.0001);

    double totalPnl = std::stod(*result->realizedPnl);
    EXPECT_NEAR(totalPnl, 5.0, 0.0001);

    EXPECT_EQ(result->firstTradeTime, 1000);
    EXPECT_EQ(result->lastTradeTime, 2000);
}

TEST(Mql4PhaseDTest, QueryOrderFillSummaryKeepsCleanDecimalFormatting) {
    StubRestClient rest;

    UserTrade t1;
    t1.symbol = "BTCUSDT";
    t1.orderId = 54321;
    t1.qty = "0.01000000";
    t1.price = "10.00000000";
    t1.realizedPnl = "0.00000000";
    t1.commission = "0.00100000";
    t1.commissionAsset = "USDT";
    t1.time = 1000;

    UserTrade t2;
    t2.symbol = "BTCUSDT";
    t2.orderId = 54321;
    t2.qty = "0.09000000";
    t2.price = "10.00000000";
    t2.realizedPnl = "0.00000000";
    t2.commission = "0.00900000";
    t2.commissionAsset = "USDT";
    t2.time = 2000;

    rest.userTradesResult = {t1, t2};

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    Orders orders(rest, cfg);

    auto result = runAwaitable(orders.queryOrderFillSummary("BTCUSDT", 54321));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->completeness, FillSummaryCompleteness::Complete);
    EXPECT_EQ(result->executedQty, "0.1");
    ASSERT_TRUE(result->avgEntryPrice.has_value());
    EXPECT_EQ(*result->avgEntryPrice, "10");
    ASSERT_TRUE(result->commission.has_value());
    EXPECT_EQ(*result->commission, "0.01");
}

TEST(Mql4PhaseDTest, QueryOrderFillSummaryMarksPartialWhenTradeHasInvalidNumbers) {
    StubRestClient rest;

    UserTrade valid;
    valid.symbol = "BTCUSDT";
    valid.orderId = 8888;
    valid.qty = "0.01";
    valid.price = "50000";
    valid.realizedPnl = "1.5";
    valid.commission = "0.2";
    valid.commissionAsset = "USDT";
    valid.time = 1000;

    UserTrade invalid;
    invalid.symbol = "BTCUSDT";
    invalid.orderId = 8888;
    invalid.qty = "bad-qty";
    invalid.price = "51000";
    invalid.realizedPnl = "2.0";
    invalid.commission = "0.1";
    invalid.commissionAsset = "USDT";
    invalid.time = 2000;

    rest.userTradesResult = {valid, invalid};

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    Orders orders(rest, cfg);

    auto result = runAwaitable(orders.queryOrderFillSummary("BTCUSDT", 8888));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->completeness, FillSummaryCompleteness::Partial);
    EXPECT_EQ(result->executedQty, "0.01");
    // On any parse failure, aggregate PnL/commission must be suppressed entirely
    // rather than emitted as a silently-undercounted partial sum.
    EXPECT_FALSE(result->realizedPnl.has_value());
    EXPECT_FALSE(result->commission.has_value());
}

TEST(Mql4PhaseDTest, QueryOrderFillSummarySuppressesPnlWhenRealizedPnlUnparseable) {
    StubRestClient rest;

    UserTrade t1;
    t1.symbol = "BTCUSDT";
    t1.orderId = 9999;
    t1.qty = "0.01";
    t1.price = "50000";
    t1.realizedPnl = "1.5";
    t1.commission = "0.2";
    t1.commissionAsset = "USDT";
    t1.time = 1000;

    UserTrade t2;
    t2.symbol = "BTCUSDT";
    t2.orderId = 9999;
    t2.qty = "0.02";
    t2.price = "50100";
    t2.realizedPnl = "not-a-number";
    t2.commission = "0.4";
    t2.commissionAsset = "USDT";
    t2.time = 2000;

    rest.userTradesResult = {t1, t2};

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    Orders orders(rest, cfg);

    auto result = runAwaitable(orders.queryOrderFillSummary("BTCUSDT", 9999));

    ASSERT_TRUE(result.has_value());
    // qty/price parsed for both trades, so executedQty is complete, but the
    // unparseable realizedPnl flags a parse error and suppresses PnL/commission.
    EXPECT_EQ(result->completeness, FillSummaryCompleteness::Partial);
    EXPECT_FALSE(result->realizedPnl.has_value());
    EXPECT_FALSE(result->commission.has_value());
}
