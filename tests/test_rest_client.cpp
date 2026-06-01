#include <gtest/gtest.h>

#include "context.h"
#include "rest/rest_client.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>

namespace {

ContextConfig makeDummyConfig() {
    ContextConfig cfg;
    cfg.apiKey = "test";
    cfg.secretKey = "test";
    cfg.testnet = true;
    cfg.threadPoolSize = 1;
    return cfg;
}

} // namespace

template <typename T>
T runAwaitable(boost::asio::io_context& ioc, boost::asio::awaitable<T> task) {
    auto fut = boost::asio::co_spawn(ioc, std::move(task), boost::asio::use_future);
    ioc.restart();
    ioc.run();
    return fut.get();
}

TEST(RestClientTest, RawParseAndParseResponseWithOnDemand) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl(boost::asio::ssl::context::tls_client);
    RestClient client(ioc, ssl, makeDummyConfig());

    const std::string body = R"({"serverTime":1710000000000,"ok":true})";
    auto parsed = client.rawParse(body);
    ASSERT_TRUE(parsed.has_value());

    auto serverTimeResult = client.parseResponse<int64_t>(body, [](simdjson::ondemand::document& doc) {
        auto obj = doc.get_object().value();
        return obj.find_field_unordered("serverTime").get_int64().value();
    });
    ASSERT_TRUE(serverTimeResult.has_value());
    EXPECT_EQ(*serverTimeResult, 1710000000000LL);
}

TEST(RestClientTest, RawParseDocumentsRemainValidAcrossSubsequentCalls) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl(boost::asio::ssl::context::tls_client);
    RestClient client(ioc, ssl, makeDummyConfig());

    auto first = client.rawParse(R"({"serverTime":1})");
    ASSERT_TRUE(first.has_value());
    auto second = client.rawParse(R"({"serverTime":2})");
    ASSERT_TRUE(second.has_value());

    auto firstObj = first->document.get_object().value();
    auto secondObj = second->document.get_object().value();
    EXPECT_EQ(firstObj.find_field_unordered("serverTime").get_int64().value(), 1);
    EXPECT_EQ(secondObj.find_field_unordered("serverTime").get_int64().value(), 2);
}

TEST(RestClientTest, ParseResponseInvalidJsonReturnsParseError) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl(boost::asio::ssl::context::tls_client);
    RestClient client(ioc, ssl, makeDummyConfig());

    auto parsed = client.parseResponse<int64_t>("{invalid-json", [](simdjson::ondemand::document& doc) {
        auto obj = doc.get_object().value();
        return obj.find_field_unordered("serverTime").get_int64().value();
    });
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().category, ErrorCategory::Parse);
}

TEST(RestClientTest, BatchOrdersRejectsOversizedPayload) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl(boost::asio::ssl::context::tls_client);
    RestClient client(ioc, ssl, makeDummyConfig());

    std::vector<OrderRequest> requests(6);
    for (auto& req : requests) {
        req.symbol = "BTCUSDT";
        req.side = OrderSide::Buy;
        req.type = OrderType::Market;
        req.positionSide = PositionSide::Both;
        req.quantity = "0.01";
    }

    auto result = runAwaitable(ioc, client.batchOrders(std::move(requests)));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, -91003);
}
