#include <gtest/gtest.h>

#include "context.h"
#include "rest/rest_client.h"

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
