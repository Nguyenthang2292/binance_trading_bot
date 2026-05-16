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
