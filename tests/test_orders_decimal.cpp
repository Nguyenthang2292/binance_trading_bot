#include <gtest/gtest.h>

#include "orders/decimal_string.h"

TEST(OrdersDecimalTest, ParseValidValues) {
    auto one = DecimalString::parse("1");
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->value(), "1");

    auto fractional = DecimalString::parse("0.001");
    ASSERT_TRUE(fractional.has_value());
    EXPECT_EQ(fractional->value(), "0.001");
}

TEST(OrdersDecimalTest, ParseRejectsInvalidValues) {
    EXPECT_FALSE(DecimalString::parse("").has_value());
    EXPECT_FALSE(DecimalString::parse("abc").has_value());
    EXPECT_FALSE(DecimalString::parse("1e-3").has_value());
    EXPECT_FALSE(DecimalString::parse("1,5").has_value());
    EXPECT_FALSE(DecimalString::parse("1.2.3").has_value());
}
