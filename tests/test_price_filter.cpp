#include <gtest/gtest.h>

#include "engine/price_filter.h"

TEST(PriceFilterTest, RoundsDownToTickAndFormatsWithoutExtraPrecision) {
    const auto price = engine::priceToTickDecimal(0.123456789, 0.0001, engine::PriceRounding::Down);

    ASSERT_TRUE(price.has_value());
    EXPECT_EQ(price->value(), "0.1234");
}

TEST(PriceFilterTest, RoundsUpToTickAndFormatsWithoutExtraPrecision) {
    const auto price = engine::priceToTickDecimal(0.123400001, 0.0001, engine::PriceRounding::Up);

    ASSERT_TRUE(price.has_value());
    EXPECT_EQ(price->value(), "0.1235");
}

TEST(PriceFilterTest, FallsBackToGenericDecimalWhenTickSizeMissing) {
    const auto price = engine::priceToTickDecimal(120.0, 0.0, engine::PriceRounding::Down);

    ASSERT_TRUE(price.has_value());
    EXPECT_EQ(price->value(), "120");
}

TEST(PriceFilterTest, QuantityFormatsToStepSizeWithoutFloatingPointArtifacts) {
    const auto qty = engine::quantityToStepDecimal(0.30000000000000004, 0.1);

    ASSERT_TRUE(qty.has_value());
    EXPECT_EQ(qty->value(), "0.3");
}

TEST(PriceFilterTest, UsesExactTickStringForDecimalCount) {
    // WR-34: the exact tick string "0.00010000" yields 4 decimals; trailing
    // zeros in the raw string must not inflate the formatted precision.
    const auto price = engine::priceToTickDecimal(
        0.123456789, 0.0001, "0.00010000", engine::PriceRounding::Down);

    ASSERT_TRUE(price.has_value());
    EXPECT_EQ(price->value(), "0.1234");
}

TEST(PriceFilterTest, IntegerTickStringYieldsNoFractionalDigits) {
    const auto price = engine::priceToTickDecimal(
        12345.6, 1.0, "1", engine::PriceRounding::Down);

    ASSERT_TRUE(price.has_value());
    EXPECT_EQ(price->value(), "12345");
}

TEST(PriceFilterTest, BlankTickRawFallsBackToDoubleDecimalCount) {
    const auto price = engine::priceToTickDecimal(
        0.123456789, 0.0001, std::string_view{}, engine::PriceRounding::Down);

    ASSERT_TRUE(price.has_value());
    EXPECT_EQ(price->value(), "0.1234");
}

TEST(PriceFilterTest, QuantityUsesExactStepString) {
    const auto qty = engine::quantityToStepDecimal(0.30000000000000004, 0.1, "0.10000000");

    ASSERT_TRUE(qty.has_value());
    EXPECT_EQ(qty->value(), "0.3");
}
