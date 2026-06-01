#include <gtest/gtest.h>

#include "ws/ws_parse_helpers.h"

#include <simdjson.h>

#include <stdexcept>

TEST(WsParseHelpersTest, ParseSideRejectsUnknownValue) {
    EXPECT_THROW((void)ws_parse::parseSide("UNKNOWN"), std::invalid_argument);
}

TEST(WsParseHelpersTest, ParseSideAcceptsBuyAndSell) {
    EXPECT_EQ(ws_parse::parseSide("BUY"), OrderSide::Buy);
    EXPECT_EQ(ws_parse::parseSide("SELL"), OrderSide::Sell);
}

TEST(WsParseHelpersTest, IntFieldParsesStringNumbers) {
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    simdjson::padded_string json(std::string(R"({"n":"123","bad":"abc"})"));
    ASSERT_FALSE(parser.parse(json).get(doc));

    EXPECT_EQ(ws_parse::intField(doc, "n", 0), 123);
    EXPECT_EQ(ws_parse::intField(doc, "bad", 7), 7);
}
