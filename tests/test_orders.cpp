#include <gtest/gtest.h>

#include "orders/irest_client.h"
#include "orders/order_journal.h"
#include "orders/orders.h"

#include <boost/asio/error.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <stdexcept>

namespace {

class StubRestClient final : public IRestClient {
public:
    RestResult<Order> newOrderResult = std::unexpected(BinanceError::fromApiResponse(-1, "not set"));
    RestResult<Order> cancelOrderResult = std::unexpected(BinanceError::fromApiResponse(-1, "not set"));
    RestResult<Order> queryOrderResult = std::unexpected(BinanceError::fromApiResponse(-1, "not set"));
    RestResult<void> cancelAllOrdersResult = RestResult<void>{};
    RestResult<std::vector<Order>> openOrdersResult = std::vector<Order>{};
    RestResult<std::vector<Order>> allOrdersResult = std::vector<Order>{};
    RestResult<LeverageResult> setLeverageResult = LeverageResult{};
    std::function<void()> onNewOrder;
    int newOrderCalls{0};
    int setLeverageCalls{0};
    int cancelAllOrdersCalls{0};
    int queryOrderByClientOrderIdCalls{0};
    int allOrdersCalls{0};
    std::string cancelAllOrdersSymbol;
    std::string queryOrderByClientOrderIdSymbol;
    std::string queryOrderByClientOrderIdValue;
    std::string allOrdersSymbol;
    std::optional<int64_t> allOrdersStartTime;
    std::optional<int64_t> allOrdersEndTime;
    int allOrdersLimit{0};
    std::string setLeverageSymbol;
    int setLeverageValue{0};

    boost::asio::awaitable<RestResult<Order>> newOrder(OrderRequest) override {
        ++newOrderCalls;
        if (onNewOrder) {
            onNewOrder();
        }
        co_return newOrderResult;
    }

    boost::asio::awaitable<RestResult<Order>> modifyOrder(OrderRequest) override {
        co_return newOrderResult;
    }

    boost::asio::awaitable<RestResult<Order>> cancelOrder(std::string, int64_t) override {
        co_return cancelOrderResult;
    }

    boost::asio::awaitable<RestResult<Order>> cancelOrderByClientOrderId(std::string, std::string) override {
        co_return cancelOrderResult;
    }

    boost::asio::awaitable<RestResult<void>> cancelAllOrders(std::string symbol) override {
        ++cancelAllOrdersCalls;
        cancelAllOrdersSymbol = std::move(symbol);
        co_return cancelAllOrdersResult;
    }

    boost::asio::awaitable<RestResult<Order>> queryOrder(std::string, int64_t) override {
        co_return queryOrderResult;
    }

    boost::asio::awaitable<RestResult<Order>> queryOrderByClientOrderId(std::string symbol, std::string clientOrderId) override {
        ++queryOrderByClientOrderIdCalls;
        queryOrderByClientOrderIdSymbol = std::move(symbol);
        queryOrderByClientOrderIdValue = std::move(clientOrderId);
        co_return queryOrderResult;
    }

    boost::asio::awaitable<RestResult<std::vector<Order>>> openOrders(std::optional<std::string>) override {
        co_return openOrdersResult;
    }

    boost::asio::awaitable<RestResult<std::vector<Order>>> allOrders(std::string symbol,
                                                                      std::optional<int64_t> startTime,
                                                                      std::optional<int64_t> endTime,
                                                                      int limit) override {
        ++allOrdersCalls;
        allOrdersSymbol = std::move(symbol);
        allOrdersStartTime = startTime;
        allOrdersEndTime = endTime;
        allOrdersLimit = limit;
        co_return allOrdersResult;
    }

    boost::asio::awaitable<RestResult<std::vector<UserTrade>>> userTrades(
        std::string,
        std::optional<int64_t>,
        std::optional<int64_t>,
        std::optional<int64_t>,
        int) override {
        co_return std::vector<UserTrade>{};
    }

    boost::asio::awaitable<RestResult<LeverageResult>> setLeverage(std::string symbol, int leverage) override {
        ++setLeverageCalls;
        setLeverageSymbol = std::move(symbol);
        setLeverageValue = leverage;
        co_return setLeverageResult;
    }

    boost::asio::awaitable<RestResult<BatchOrderResult>> batchOrders(std::vector<OrderRequest>) override {
        BatchOrderResult result;
        co_return result;
    }
};

class CaptureJournal final : public OrderJournal {
public:
    std::optional<JournalEntry> lastEntry;

    std::expected<void, BinanceError> recordIntent(JournalEntry entry) override {
        lastEntry = entry;
        return {};
    }

    std::expected<void, BinanceError> updateState(CorrelationId, PlacementState, std::optional<int64_t>) override {
        return {};
    }

    std::expected<std::vector<JournalEntry>, BinanceError> pendingReconcile() override {
        return std::vector<JournalEntry>{};
    }

    std::expected<std::optional<JournalEntry>, BinanceError> findByClientOrderId(const ClientOrderId&) override {
        return std::optional<JournalEntry>{};
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

TEST(OrdersTest, SetLeverageDelegatesToRestClient) {
    StubRestClient rest;
    rest.setLeverageResult = LeverageResult{
        .symbol = "BTCUSDT",
        .leverage = 13,
        .maxNotionalValue = 100000.0,
    };

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    Orders orders(rest, cfg);

    auto result = runAwaitable(orders.setLeverage("BTCUSDT", 13));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(rest.setLeverageCalls, 1);
    EXPECT_EQ(rest.setLeverageSymbol, "BTCUSDT");
    EXPECT_EQ(rest.setLeverageValue, 13);
    EXPECT_EQ(result->leverage, 13);
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

TEST(OrdersTest, MarketMapsMinus1007ToTimeoutCategory) {
    StubRestClient rest;
    rest.newOrderResult = std::unexpected(BinanceError{
        .category = ErrorCategory::Api,
        .code = -1007,
        .message = "timeout",
    });

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.allowBestEffortJournal = true;
    Orders orders(rest, cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
        .clientOrderId = std::string("cid-timeout-1"),
    };

    auto result = runAwaitable(orders.market(draft));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->state, PlacementState::UnknownPendingReconcile);
    ASSERT_TRUE(result->errorCategory.has_value());
    EXPECT_EQ(*result->errorCategory, OrderErrorCategory::Timeout);
    ASSERT_TRUE(result->endpoint.has_value());
    EXPECT_EQ(*result->endpoint, "/fapi/v1/order");
    ASSERT_TRUE(result->rawResponseBody.has_value());
    EXPECT_EQ(*result->rawResponseBody, "timeout");
}

TEST(OrdersTest, MarketRecordsNonEmptyRequestParamsInJournal) {
    StubRestClient rest;
    Order placed;
    placed.symbol = "BTCUSDT";
    placed.orderId = 1001;
    placed.status = "NEW";
    placed.clientOrderId = "cid-journal-1";
    rest.newOrderResult = placed;

    auto journal = std::make_shared<CaptureJournal>();

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.journal = journal;
    cfg.journalIsDurable = true;
    Orders orders(rest, cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
        .clientOrderId = std::string("cid-journal-1"),
    };

    auto result = runAwaitable(orders.market(draft));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(journal->lastEntry.has_value());
    EXPECT_NE(journal->lastEntry->requestParams.find("symbol=BTCUSDT"), std::string::npos);
    EXPECT_NE(journal->lastEntry->requestParams.find("newClientOrderId=cid-journal-1"), std::string::npos);
}

TEST(OrdersTest, MarketRecordsJournalBeforeSendingOrder) {
    StubRestClient rest;
    Order placed;
    placed.symbol = "BTCUSDT";
    placed.orderId = 1002;
    placed.status = "NEW";
    placed.clientOrderId = "cid-ordering-1";
    rest.newOrderResult = placed;

    auto journal = std::make_shared<CaptureJournal>();
    rest.onNewOrder = [&journal] {
        EXPECT_TRUE(journal->lastEntry.has_value());
    };

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.journal = journal;
    cfg.journalIsDurable = true;
    Orders orders(rest, cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
        .clientOrderId = std::string("cid-ordering-1"),
    };

    auto result = runAwaitable(orders.market(draft));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(rest.newOrderCalls, 1);
    ASSERT_TRUE(journal->lastEntry.has_value());
    EXPECT_EQ(journal->lastEntry->clientOrderId, "cid-ordering-1");
}

TEST(OrdersTest, DurableJournalRequiredWhenBestEffortDisabled) {
    StubRestClient rest;
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.allowBestEffortJournal = false;
    cfg.journalIsDurable = false;
    Orders orders(rest, cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
        .clientOrderId = std::string("cid-required-1"),
    };

    auto result = runAwaitable(orders.market(draft));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->state, PlacementState::Rejected);
    ASSERT_TRUE(result->errorCategory.has_value());
    EXPECT_EQ(*result->errorCategory, OrderErrorCategory::Journal);
}

TEST(OrdersTest, DurableJournalWriteFailureBlocksPlacement) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto journalDirectory = std::filesystem::temp_directory_path()
        / ("btb_orders_journal_dir_" + std::to_string(suffix));
    std::filesystem::create_directories(journalDirectory);

    StubRestClient rest;
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.journalIsDurable = true;
    cfg.journalPath = journalDirectory.string();
    Orders orders(rest, cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
        .clientOrderId = std::string("cid-journal-fail-1"),
    };

    auto result = runAwaitable(orders.market(draft));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->state, PlacementState::Rejected);
    ASSERT_TRUE(result->errorCategory.has_value());
    EXPECT_EQ(*result->errorCategory, OrderErrorCategory::Journal);
    EXPECT_EQ(rest.newOrderCalls, 0);

    std::filesystem::remove_all(journalDirectory);
}

TEST(OrdersTest, OperationAbortedBeforeSendMapsToCanceledBeforeSend) {
    StubRestClient rest;
    rest.newOrderResult = std::unexpected(BinanceError::fromNetwork(
        boost::asio::error::operation_aborted,
        NetworkErrorPhase::BeforeSend));

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.allowBestEffortJournal = true;
    Orders orders(rest, cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
        .clientOrderId = std::string("cid-cancel-before-send-1"),
    };

    auto result = runAwaitable(orders.market(draft));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->state, PlacementState::Rejected);
    ASSERT_TRUE(result->errorCategory.has_value());
    EXPECT_EQ(*result->errorCategory, OrderErrorCategory::CanceledBeforeSend);
}

TEST(OrdersTest, NetworkErrorMessageAloneDoesNotMapToCanceledBeforeSend) {
    StubRestClient rest;
    rest.newOrderResult = std::unexpected(BinanceError{
        .category = ErrorCategory::Network,
        .code = 0,
        .message = "operation aborted",
    });

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.allowBestEffortJournal = true;
    Orders orders(rest, cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
        .clientOrderId = std::string("cid-message-only-1"),
    };

    auto result = runAwaitable(orders.market(draft));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->state, PlacementState::UnknownPendingReconcile);
    ASSERT_TRUE(result->errorCategory.has_value());
    EXPECT_EQ(*result->errorCategory, OrderErrorCategory::Network);
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

TEST(OrdersTest, CancelAllNormalDelegatesSymbolScopedCancelAll) {
    StubRestClient rest;
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    Orders orders(rest, cfg);

    auto result = runAwaitable(orders.cancelAllNormal("ETHUSDT"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(rest.cancelAllOrdersCalls, 1);
    EXPECT_EQ(rest.cancelAllOrdersSymbol, "ETHUSDT");
}

TEST(OrdersTest, QueryAllNormalRejectsSevenDayOrWiderWindowBeforeRestCall) {
    StubRestClient rest;
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    Orders orders(rest, cfg);

    constexpr int64_t kSevenDaysMs = 7LL * 24LL * 60LL * 60LL * 1000LL;
    auto result = runAwaitable(orders.queryAllNormal("BTCUSDT", 1000, 1000 + kSevenDaysMs, 500));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(rest.allOrdersCalls, 0);
    EXPECT_EQ(result.error().code, -90007);
}

TEST(OrdersTest, QueryAllNormalPassesValidWindowAndLimitToRest) {
    StubRestClient rest;
    Order order;
    order.symbol = "ETHUSDT";
    order.orderId = 321;
    order.clientOrderId = "cid-history-1";
    rest.allOrdersResult = std::vector<Order>{order};

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    Orders orders(rest, cfg);

    auto result = runAwaitable(orders.queryAllNormal("ETHUSDT", 1000, 2000, 42));

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1);
    EXPECT_EQ(rest.allOrdersCalls, 1);
    EXPECT_EQ(rest.allOrdersSymbol, "ETHUSDT");
    ASSERT_TRUE(rest.allOrdersStartTime.has_value());
    ASSERT_TRUE(rest.allOrdersEndTime.has_value());
    EXPECT_EQ(*rest.allOrdersStartTime, 1000);
    EXPECT_EQ(*rest.allOrdersEndTime, 2000);
    EXPECT_EQ(rest.allOrdersLimit, 42);
}

TEST(OrdersTest, ReconcilePrimitiveCanQueryClientIdThenHistory) {
    StubRestClient rest;
    rest.queryOrderResult = std::unexpected(BinanceError::fromApiResponse(-2013, "Order does not exist"));

    Order historical;
    historical.symbol = "BTCUSDT";
    historical.orderId = 987;
    historical.clientOrderId = "cid-reconcile-1";
    historical.status = "FILLED";
    rest.allOrdersResult = std::vector<Order>{historical};

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    Orders orders(rest, cfg);

    auto byClientId = runAwaitable(orders.queryNormalByClientOrderId("BTCUSDT", "cid-reconcile-1"));
    ASSERT_FALSE(byClientId.has_value());
    EXPECT_EQ(rest.queryOrderByClientOrderIdCalls, 1);
    EXPECT_EQ(rest.queryOrderByClientOrderIdSymbol, "BTCUSDT");
    EXPECT_EQ(rest.queryOrderByClientOrderIdValue, "cid-reconcile-1");

    auto history = runAwaitable(orders.queryAllNormal("BTCUSDT", 1000, 2000, 500));
    ASSERT_TRUE(history.has_value());
    ASSERT_EQ(history->size(), 1);
    EXPECT_EQ((*history)[0].clientOrderId, "cid-reconcile-1");
    EXPECT_EQ((*history)[0].orderId, 987);
}

TEST(OrdersTest, PlacementResultCarriesValidationReportOnSuccessAndFailure) {
    StubRestClient rest;
    Order placed;
    placed.symbol = "BTCUSDT";
    placed.orderId = 777;
    placed.status = "NEW";
    placed.clientOrderId = "cid-validation-success-1";
    rest.newOrderResult = placed;

    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.allowBestEffortJournal = true;
    cfg.positionMode = PositionMode::Unknown;
    Orders orders(rest, cfg);

    MarketOrderDraft successDraft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
        .clientOrderId = std::string("cid-validation-success-1"),
    };
    auto success = runAwaitable(orders.market(successDraft));
    ASSERT_TRUE(success.has_value());
    EXPECT_FALSE(success->validation.hasErrors());
    EXPECT_FALSE(success->validation.issues.empty());

    MarketOrderDraft failureDraft{
        .symbol = "",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
        .clientOrderId = std::string("cid-validation-failure-1"),
    };
    auto failure = runAwaitable(orders.market(failureDraft));
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->state, PlacementState::Rejected);
    EXPECT_TRUE(failure->validation.hasErrors());
}
