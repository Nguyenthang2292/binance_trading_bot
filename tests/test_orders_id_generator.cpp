#include <gtest/gtest.h>

#include "orders/order_id_generator.h"

#include <regex>

TEST(OrderIdGeneratorTest, GeneratedClientIdMatchesConstraints) {
    OrderIdGenerator gen("strat01");
    auto id = gen.generateClientOrderId();
    ASSERT_TRUE(id.has_value());
    EXPECT_LE(id->size(), 36U);
    EXPECT_TRUE(id->starts_with("btb_strat01_"));
    EXPECT_TRUE(gen.validateClientOrderId(*id).has_value());
}

TEST(OrderIdGeneratorTest, GeneratedClientIdContainsEightHexEntropyChars) {
    OrderIdGenerator gen("alpha01");
    auto id = gen.generateClientOrderId();
    ASSERT_TRUE(id.has_value());
    EXPECT_TRUE(std::regex_match(*id, std::regex("^btb_alpha01_[0-9]{13}_[0-9a-f]{8}$")));
}

TEST(OrderIdGeneratorTest, RejectsInvalidNamespace) {
    OrderIdGenerator gen("bad-ns");
    auto id = gen.generateClientOrderId();
    EXPECT_FALSE(id.has_value());
}

TEST(OrderIdGeneratorTest, RejectsInvalidClientIdOverride) {
    OrderIdGenerator gen("alpha01");
    auto valid = gen.validateClientOrderId("ok_id-1");
    EXPECT_TRUE(valid.has_value());

    auto invalid = gen.validateClientOrderId("bad id");
    EXPECT_FALSE(invalid.has_value());
}
