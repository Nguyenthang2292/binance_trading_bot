#include <gtest/gtest.h>

#include "types/account.h"
#include "types/error.h"
#include "types/events.h"
#include "types/market.h"
#include "types/trade.h"

TEST(ErrorTypesTest, HttpStatusMapsCategories) {
    EXPECT_EQ(BinanceError::fromHttp(429, "too many requests").category, ErrorCategory::RateLimit);
    EXPECT_EQ(BinanceError::fromHttp(418, "ip banned").category, ErrorCategory::RateLimit);
    EXPECT_EQ(BinanceError::fromHttp(401, "bad key").category, ErrorCategory::Auth);
    EXPECT_EQ(BinanceError::fromHttp(500, "server error").category, ErrorCategory::Api);
}

TEST(ErrorTypesTest, BinanceAuthCodesMapAuthCategory) {
    const auto err = BinanceError::fromApiResponse(-2015, "Invalid API-key");
    EXPECT_EQ(err.category, ErrorCategory::Auth);
    EXPECT_EQ(err.code, -2015);
    EXPECT_NE(err.toString().find("Invalid API-key"), std::string::npos);
}

TEST(ErrorTypesTest, HttpAuthJsonPreservesBinanceCodeAndMessage) {
    const auto err = BinanceError::fromHttp(
        401,
        R"({"code":-2015,"msg":"Invalid API-key, IP, or permissions for action."})");
    EXPECT_EQ(err.category, ErrorCategory::Auth);
    EXPECT_EQ(err.code, -2015);
    EXPECT_EQ(err.message, "Invalid API-key, IP, or permissions for action.");
}

TEST(TypesTest, KlineKeepsLegacyFieldAliases) {
    Kline k;
    k.quoteVolume = 123.4;
    k.quoteAssetVolume = k.quoteVolume;
    k.tradeCount = 42;
    k.numberOfTrades = k.tradeCount;

    EXPECT_DOUBLE_EQ(k.quoteAssetVolume, k.quoteVolume);
    EXPECT_EQ(k.numberOfTrades, k.tradeCount);
}

TEST(TypesTest, MarketAndUserEventVariantsHoldExpectedTypes) {
    MarketEvent market = AggTradeEvent{.symbol = "BTCUSDT", .price = 50000.0, .qty = 1.0};
    ASSERT_TRUE(std::holds_alternative<AggTradeEvent>(market));

    UserDataEvent user = MarginCallEvent{};
    ASSERT_TRUE(std::holds_alternative<MarginCallEvent>(user));
}
