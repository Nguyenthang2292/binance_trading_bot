#include <gtest/gtest.h>

#include "strategy/indicators/atr.h"

#include <limits>

TEST(AtrIndicatorTest, ComputesWilderAtr) {
    std::vector<Kline> klines(4);
    klines[0].high = 11.0;
    klines[0].low = 9.0;
    klines[0].close = 10.0;
    klines[1].high = 12.0;
    klines[1].low = 10.0;
    klines[1].close = 11.0;
    klines[2].high = 13.0;
    klines[2].low = 11.0;
    klines[2].close = 12.0;
    klines[3].high = 14.0;
    klines[3].low = 12.0;
    klines[3].close = 13.0;

    const auto values = strategy::indicators::atr(klines, 2);
    ASSERT_EQ(values.size(), 4u);
    EXPECT_DOUBLE_EQ(values[2], 2.0);
    EXPECT_DOUBLE_EQ(values[3], 2.0);
    EXPECT_DOUBLE_EQ(strategy::indicators::lastAtr(klines, 2), 2.0);
}

TEST(AtrIndicatorTest, InsufficientDataReturnsZero) {
    std::vector<Kline> klines(2);
    const auto values = strategy::indicators::atr(klines, 14);
    ASSERT_EQ(values.size(), 2u);
    EXPECT_DOUBLE_EQ(values[0], 0.0);
    EXPECT_DOUBLE_EQ(values[1], 0.0);
    EXPECT_DOUBLE_EQ(strategy::indicators::lastAtr(klines, 14), 0.0);
}

TEST(AtrIndicatorTest, MalformedOhlcReturnsZeroes) {
    std::vector<Kline> klines(4);
    for (auto& kline : klines) {
        kline.high = 12.0;
        kline.low = 10.0;
        kline.close = 11.0;
    }
    klines[2].high = std::numeric_limits<double>::quiet_NaN();

    const auto values = strategy::indicators::atr(klines, 2);
    ASSERT_EQ(values.size(), 4u);
    EXPECT_DOUBLE_EQ(values[2], 0.0);
    EXPECT_DOUBLE_EQ(values[3], 0.0);
    EXPECT_DOUBLE_EQ(strategy::indicators::lastAtr(klines, 2), 0.0);
}

TEST(AtrIndicatorTest, HugePeriodDoesNotOverflowBeforeSizeCheck) {
    std::vector<Kline> klines(4);
    const auto values = strategy::indicators::atr(klines, std::numeric_limits<int>::max());
    ASSERT_EQ(values.size(), 4u);
    EXPECT_DOUBLE_EQ(strategy::indicators::lastAtr(klines, std::numeric_limits<int>::max()), 0.0);
}

