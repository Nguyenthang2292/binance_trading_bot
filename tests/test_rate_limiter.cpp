#include <gtest/gtest.h>

#include "rest/rate_limiter.h"
#include "transport/rate_limit_headers.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <limits>

TEST(RateLimiterTest, KlineWeightBoundaries) {
    EXPECT_EQ(RateLimiter::klineWeight(1), 1);
    EXPECT_EQ(RateLimiter::klineWeight(99), 1);
    EXPECT_EQ(RateLimiter::klineWeight(100), 2);
    EXPECT_EQ(RateLimiter::klineWeight(499), 2);
    EXPECT_EQ(RateLimiter::klineWeight(500), 5);
    EXPECT_EQ(RateLimiter::klineWeight(1000), 5);
    EXPECT_EQ(RateLimiter::klineWeight(1001), 10);
}

TEST(RateLimiterTest, DepthWeightBoundaries) {
    EXPECT_EQ(RateLimiter::depthWeight(5), 2);
    EXPECT_EQ(RateLimiter::depthWeight(50), 2);
    EXPECT_EQ(RateLimiter::depthWeight(51), 5);
    EXPECT_EQ(RateLimiter::depthWeight(100), 5);
    EXPECT_EQ(RateLimiter::depthWeight(101), 10);
    EXPECT_EQ(RateLimiter::depthWeight(500), 10);
    EXPECT_EQ(RateLimiter::depthWeight(501), 20);
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
    applyRateLimitHeader(usage, "X-MBX-USED-WEIGHT-1M", " 1920\r");

    EXPECT_EQ(usage.usedWeight1m, 1920);
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

TEST(RateLimiterTest, RateLimitHeaderParserClampsLargeValues) {
    RateLimitHeaderUsage usage;
    applyRateLimitHeader(usage, "X-MBX-USED-WEIGHT-1M", "999999999999999999999\r");
    EXPECT_EQ(usage.usedWeight1m, std::numeric_limits<int>::max());
}

TEST(RateLimiterTest, ReleaseReturnsReservationBudget) {
    RateLimiter::Limits limits;
    limits.requestWeightPerMinute = 1;
    limits.ordersPerMinute = 1;
    limits.ordersPer10Seconds = 1;
    limits.safetyRatio = 1.0;
    RateLimiter limiter(limits);

    boost::asio::io_context ioc;
    auto future = boost::asio::co_spawn(
        ioc,
        [&limiter]() -> boost::asio::awaitable<void> {
            RateLimiter::Cost cost{.requestWeight = 1, .orders1m = 0, .orders10s = 0};
            co_await limiter.acquire(cost);
            limiter.release(cost);
            co_await limiter.acquire(cost);
            co_return;
        }(),
        boost::asio::use_future);
    ioc.run();
    EXPECT_NO_THROW(future.get());
}
