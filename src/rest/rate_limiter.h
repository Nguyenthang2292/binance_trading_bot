#pragma once

#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <chrono>

class RateLimiter {
public:
    struct Limits {
        int requestWeightPerMinute = 2400;
        int ordersPerMinute = 1200;
        int ordersPer10Seconds = 300;
    };

    RateLimiter();
    explicit RateLimiter(Limits limits);

    void update(int usedWeight, int usedOrders);
    void update(int usedWeight, int usedOrders, int usedOrders10s);
    bool isNearLimit() const;
    boost::asio::awaitable<void> waitIfNeeded();

private:
    Limits m_limits;
    std::atomic<int> m_usedWeight{0};
    std::atomic<int> m_usedOrders{0};
    std::atomic<int> m_usedOrders10s{0};
    std::atomic<std::chrono::steady_clock::time_point> m_windowStart;
    std::atomic<std::chrono::steady_clock::time_point> m_order10sWindowStart;
};
