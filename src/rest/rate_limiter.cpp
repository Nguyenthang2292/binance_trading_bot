#include "rest/rate_limiter.h"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

RateLimiter::RateLimiter() : RateLimiter(Limits{}) {}

RateLimiter::RateLimiter(Limits limits)
    : m_limits(limits), m_windowStart(std::chrono::steady_clock::now()) {}

void RateLimiter::update(int usedWeight, int usedOrders) {
    if (usedWeight >= 0) {
        m_usedWeight = usedWeight;
    }
    if (usedOrders >= 0) {
        m_usedOrders = usedOrders;
    }

    const auto now = std::chrono::steady_clock::now();
    auto windowStart = m_windowStart.load();
    if (now - windowStart >= std::chrono::minutes(1)) {
        m_windowStart.store(now);
        m_usedWeight = 0;
        m_usedOrders = 0;
    }
}

bool RateLimiter::isNearLimit() const {
    return m_usedWeight.load() >= static_cast<int>(m_limits.requestWeightPerMinute * 0.8) ||
           m_usedOrders.load() >= static_cast<int>(m_limits.ordersPerMinute * 0.8);
}

boost::asio::awaitable<void> RateLimiter::waitIfNeeded() {
    const bool atWeightLimit = m_usedWeight.load() >= m_limits.requestWeightPerMinute;
    const bool atOrderLimit = m_usedOrders.load() >= m_limits.ordersPerMinute;
    if (!atWeightLimit && !atOrderLimit) {
        co_return;
    }

    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor);
    const auto elapsed = std::chrono::steady_clock::now() - m_windowStart.load();
    const auto delay = elapsed >= std::chrono::minutes(1)
        ? std::chrono::milliseconds(1)
        : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::minutes(1) - elapsed);
    timer.expires_after(delay);
    co_await timer.async_wait(boost::asio::use_awaitable);

    m_windowStart.store(std::chrono::steady_clock::now());
    m_usedWeight = 0;
    m_usedOrders = 0;
}
