#include <gtest/gtest.h>

#include "ws/ws_parse_helpers.h"

#include <stdexcept>

TEST(WsParseHelpersTest, ParseSideRejectsUnknownValue) {
    EXPECT_THROW((void)ws_parse::parseSide("UNKNOWN"), std::invalid_argument);
}

TEST(WsParseHelpersTest, ParseSideAcceptsBuyAndSell) {
    EXPECT_EQ(ws_parse::parseSide("BUY"), OrderSide::Buy);
    EXPECT_EQ(ws_parse::parseSide("SELL"), OrderSide::Sell);
}
