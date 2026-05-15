#include <gtest/gtest.h>

#include "scanner/kline_cache.h"

#include <vector>

TEST(KlineCacheTest, InsertReplaceAndSnapshot) {
    scanner::KlineCache cache(3);
    Kline first;
    first.openTime = 1000;
    first.close = 10.0;
    cache.update("BTCUSDT", "15m", first);

    auto snap = cache.snapshot("BTCUSDT", "15m");
    ASSERT_TRUE(snap.has_value());
    ASSERT_EQ(snap->size(), 1u);
    EXPECT_EQ((*snap)[0].close, 10.0);

    Kline replacement = first;
    replacement.close = 11.0;
    cache.update("BTCUSDT", "15m", replacement);

    snap = cache.snapshot("BTCUSDT", "15m");
    ASSERT_TRUE(snap.has_value());
    ASSERT_EQ(snap->size(), 1u);
    EXPECT_EQ((*snap)[0].close, 11.0);
}

TEST(KlineCacheTest, RotatesFixedBuffer) {
    scanner::KlineCache cache(2);
    for (int i = 0; i < 3; ++i) {
        Kline k;
        k.openTime = 1000 + i;
        k.close = static_cast<double>(i);
        cache.update("ETHUSDT", "30m", k);
    }

    auto snap = cache.snapshot("ETHUSDT", "30m");
    ASSERT_TRUE(snap.has_value());
    ASSERT_EQ(snap->size(), 2u);
    EXPECT_EQ((*snap)[0].openTime, 1001);
    EXPECT_EQ((*snap)[1].openTime, 1002);
}

TEST(KlineCacheTest, MissingLookupReturnsNullopt) {
    scanner::KlineCache cache(3);
    EXPECT_FALSE(cache.snapshot("NONE", "15m").has_value());
}

TEST(KlineCacheTest, MergeKeepsAscendingOrderAndDeduplicates) {
    scanner::KlineCache cache(4);

    std::vector<Kline> newestPartial;
    for (int i = 0; i < 3; ++i) {
        Kline k;
        k.openTime = 2000 + i;
        k.close = static_cast<double>(100 + i);
        newestPartial.push_back(k);
    }
    cache.merge("BTCUSDT", "15m", newestPartial);

    std::vector<Kline> fullRange;
    for (int i = 0; i < 5; ++i) {
        Kline k;
        k.openTime = 1998 + i;
        k.close = static_cast<double>(10 + i);
        fullRange.push_back(k);
    }
    cache.merge("BTCUSDT", "15m", fullRange);

    auto snap = cache.snapshot("BTCUSDT", "15m");
    ASSERT_TRUE(snap.has_value());
    ASSERT_EQ(snap->size(), 4u);
    EXPECT_EQ((*snap)[0].openTime, 1999);
    EXPECT_EQ((*snap)[1].openTime, 2000);
    EXPECT_EQ((*snap)[2].openTime, 2001);
    EXPECT_EQ((*snap)[3].openTime, 2002);
}
