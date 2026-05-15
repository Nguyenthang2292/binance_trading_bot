#pragma once

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <mutex>

class RateLimiter {
public:
    struct Limits {
        int requestWeightPerMinute = 2400;
        int ordersPerMinute = 1200;
        int ordersPer10Seconds = 300;
        double safetyRatio = 0.95;
    };

    struct Cost {
        int requestWeight = 1;
        int orders1m = 0;
        int orders10s = 0;
    };

    RateLimiter();
    explicit RateLimiter(Limits limits);

    static int klineWeight(int limit);

    boost::asio::awaitable<void> acquire(Cost cost);
    void updateFromHeaders(int usedWeight, int usedOrders1m, int usedOrders10s);
    void penalize(std::chrono::milliseconds delay);

    // Backward-compatible API used by existing code/tests.
    void update(int usedWeight, int usedOrders);
    void update(int usedWeight, int usedOrders, int usedOrders10s);
    bool isNearLimit() const;
    boost::asio::awaitable<void> waitIfNeeded();

private:
    void refreshWindowsLocked(std::chrono::steady_clock::time_point now);
    static int safeBudget(int limit, double safetyRatio);

    Limits m_limits;
    mutable std::mutex m_mutex;

    int m_headerUsedWeight{0};
    int m_headerUsedOrders1m{0};
    int m_headerUsedOrders10s{0};
    int m_reservedWeight{0};
    int m_reservedOrders1m{0};
    int m_reservedOrders10s{0};

    std::chrono::steady_clock::time_point m_windowStart;
    std::chrono::steady_clock::time_point m_order10sWindowStart;
    std::chrono::steady_clock::time_point m_penaltyUntil;
};
