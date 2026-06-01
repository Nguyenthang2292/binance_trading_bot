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

TEST(ErrorTypesTest, HttpRawBodyMessageIsSanitizedAndCapped) {
    std::string body = "line1\nline2\t";
    body.push_back('\x01');
    body.append(1100, 'x');

    const auto err = BinanceError::fromHttp(500, body);
    EXPECT_EQ(err.category, ErrorCategory::Api);
    EXPECT_EQ(err.message.find('\n'), std::string::npos);
    EXPECT_EQ(err.message.find('\t'), std::string::npos);
    EXPECT_NE(err.message.find('?'), std::string::npos);
    EXPECT_NE(err.message.find("[truncated]"), std::string::npos);
    EXPECT_LE(err.message.size(), 1040U);
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

TEST(TypesTest, ExchangeSymbolKeepsDistinctFilterModels) {
    ExchangeSymbol symbol;
    symbol.priceFilter = ExchangePriceFilter{.minPrice = 0.01, .maxPrice = 100000.0, .tickSize = 0.1};
    symbol.lotSize = ExchangeLotSizeFilter{.minQty = 0.001, .maxQty = 100.0, .stepSize = 0.001};
    symbol.marketLotSize = ExchangeLotSizeFilter{.minQty = 0.01, .maxQty = 50.0, .stepSize = 0.01};
    symbol.minNotionalFilter = ExchangeNotionalFilter{
        .minNotional = 5.0,
        .maxNotional = 0.0,
        .applyMinToMarket = true,
        .applyMaxToMarket = false,
        .avgPriceMins = 5,
    };

    ASSERT_TRUE(symbol.priceFilter.has_value());
    ASSERT_TRUE(symbol.lotSize.has_value());
    ASSERT_TRUE(symbol.marketLotSize.has_value());
    EXPECT_DOUBLE_EQ(symbol.lotSize->stepSize, 0.001);
    EXPECT_DOUBLE_EQ(symbol.marketLotSize->stepSize, 0.01);
    EXPECT_TRUE(symbol.minNotionalFilter->applyMinToMarket);
}

TEST(TypesTest, BookTickerCarriesSequencingMetadata) {
    BookTickerEvent event;
    event.updateId = 123;
    event.eventTime = 456;
    event.transactTime = 789;

    EXPECT_EQ(event.updateId, 123);
    EXPECT_EQ(event.eventTime, 456);
    EXPECT_EQ(event.transactTime, 789);
}

TEST(TypesTest, MarketEventCanCarryUnknownPayloadMetadata) {
    MarketEvent event = UnknownMarketEvent{.stream = "btcusdt@unknown", .eventType = "mystery"};
    ASSERT_TRUE(std::holds_alternative<UnknownMarketEvent>(event));
    EXPECT_EQ(std::get<UnknownMarketEvent>(event).stream, "btcusdt@unknown");
}

TEST(TypesTest, LeverageBracketPreservesRawMetadata) {
    SymbolLeverageBrackets brackets;
    brackets.notionalCoef = 1.5;
    brackets.notionalCoefRaw = "1.50000000";
    brackets.brackets.push_back(LeverageBracketTier{
        .bracket = 1,
        .initialLeverage = 125,
        .notionalCap = 50000.0,
        .notionalCapRaw = "50000.00000000",
        .notionalFloor = 0.0,
        .notionalFloorRaw = "0.00000000",
        .maintMarginRatio = 0.004,
        .maintMarginRatioRaw = "0.00400000",
        .cum = 0.0,
        .cumRaw = "0.00000000",
    });

    ASSERT_EQ(brackets.brackets.size(), 1U);
    EXPECT_EQ(brackets.notionalCoefRaw, "1.50000000");
    EXPECT_EQ(brackets.brackets.front().maintMarginRatioRaw, "0.00400000");
}
