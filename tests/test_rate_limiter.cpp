#include <gtest/gtest.h>

#include "rest/rate_limiter.h"
#include "transport/rate_limit_headers.h"

TEST(RateLimiterTest, TracksTenSecondOrderCount) {
    RateLimiter::Limits limits;
    limits.requestWeightPerMinute = 100;
    limits.ordersPerMinute = 100;
    limits.ordersPer10Seconds = 10;
    RateLimiter limiter(limits);

    limiter.update(0, 0, 7);
    EXPECT_FALSE(limiter.isNearLimit());

    limiter.update(0, 0, 8);
    EXPECT_TRUE(limiter.isNearLimit());
}

TEST(RateLimiterTest, ParsesBinanceRateLimitHeadersCaseInsensitively) {
    RateLimitHeaderUsage usage;

    applyRateLimitHeader(usage, "x-mbx-used-weight-1m", "1919");
    applyRateLimitHeader(usage, "X-MBX-ORDER-COUNT-1M", "950");
    applyRateLimitHeader(usage, "X-MBX-ORDER-COUNT-10S", "240");
    applyRateLimitHeader(usage, "X-MBX-ORDER-COUNT-10S", "not-a-number");

    EXPECT_EQ(usage.usedWeight1m, 1919);
    EXPECT_EQ(usage.usedOrders1m, 950);
    EXPECT_EQ(usage.usedOrders10s, 240);

    RateLimiter::Limits limits;
    limits.requestWeightPerMinute = 2400;
    limits.ordersPerMinute = 1200;
    limits.ordersPer10Seconds = 300;
    RateLimiter limiter(limits);
    limiter.update(usage.usedWeight1m, usage.usedOrders1m, usage.usedOrders10s);

    EXPECT_TRUE(limiter.isNearLimit());
}
