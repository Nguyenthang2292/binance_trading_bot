#include <gtest/gtest.h>

#include "rest/rate_limiter.h"
#include "transport/rate_limit_headers.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>

TEST(RateLimiterTest, KlineWeightBoundaries) {
    EXPECT_EQ(RateLimiter::klineWeight(1), 1);
    EXPECT_EQ(RateLimiter::klineWeight(99), 1);
    EXPECT_EQ(RateLimiter::klineWeight(100), 2);
    EXPECT_EQ(RateLimiter::klineWeight(499), 2);
    EXPECT_EQ(RateLimiter::klineWeight(500), 5);
    EXPECT_EQ(RateLimiter::klineWeight(1000), 5);
    EXPECT_EQ(RateLimiter::klineWeight(1001), 10);
}

TEST(RateLimiterTest, PenalizeDelaysAcquire) {
    RateLimiter::Limits limits;
    limits.requestWeightPerMinute = 2400;
    limits.ordersPerMinute = 1200;
    limits.ordersPer10Seconds = 300;
    limits.safetyRatio = 1.0;

    RateLimiter limiter(limits);
    limiter.penalize(std::chrono::milliseconds(50));

    boost::asio::io_context ioc;
    const auto startedAt = std::chrono::steady_clock::now();
    auto future = boost::asio::co_spawn(
        ioc,
        limiter.acquire(RateLimiter::Cost{}),
        boost::asio::use_future);
    ioc.run();
    future.get();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);

    EXPECT_GE(elapsed.count(), 20);
}

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
