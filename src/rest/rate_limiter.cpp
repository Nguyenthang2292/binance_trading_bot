#include "rest/rate_limiter.h"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>

RateLimiter::RateLimiter() : RateLimiter(Limits{}) {}

RateLimiter::RateLimiter(Limits limits)
    : m_limits(limits),
      m_windowStart(std::chrono::steady_clock::now()),
      m_order10sWindowStart(std::chrono::steady_clock::now()) {}

void RateLimiter::update(int usedWeight, int usedOrders) {
    update(usedWeight, usedOrders, -1);
}

void RateLimiter::update(int usedWeight, int usedOrders, int usedOrders10s) {
    if (usedWeight >= 0) {
        m_usedWeight = usedWeight;
    }
    if (usedOrders >= 0) {
        m_usedOrders = usedOrders;
    }
    if (usedOrders10s >= 0) {
        m_usedOrders10s = usedOrders10s;
    }

    const auto now = std::chrono::steady_clock::now();
    auto windowStart = m_windowStart.load();
    if (now - windowStart >= std::chrono::minutes(1)) {
        m_windowStart.store(now);
        m_usedWeight = 0;
        m_usedOrders = 0;
    }
    auto order10sWindowStart = m_order10sWindowStart.load();
    if (now - order10sWindowStart >= std::chrono::seconds(10)) {
        m_order10sWindowStart.store(now);
        m_usedOrders10s = 0;
    }
}

bool RateLimiter::isNearLimit() const {
    return m_usedWeight.load() >= static_cast<int>(m_limits.requestWeightPerMinute * 0.8) ||
           m_usedOrders.load() >= static_cast<int>(m_limits.ordersPerMinute * 0.8) ||
           m_usedOrders10s.load() >= static_cast<int>(m_limits.ordersPer10Seconds * 0.8);
}

boost::asio::awaitable<void> RateLimiter::waitIfNeeded() {
    const bool atWeightLimit = m_usedWeight.load() >= m_limits.requestWeightPerMinute;
    const bool atOrderLimit = m_usedOrders.load() >= m_limits.ordersPerMinute;
    const bool atOrder10sLimit = m_usedOrders10s.load() >= m_limits.ordersPer10Seconds;
    if (!atWeightLimit && !atOrderLimit && !atOrder10sLimit) {
        co_return;
    }

    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor);
    const auto now = std::chrono::steady_clock::now();
    const auto minuteElapsed = now - m_windowStart.load();
    const auto order10sElapsed = now - m_order10sWindowStart.load();
    std::chrono::milliseconds delay{1};
    if (atWeightLimit || atOrderLimit) {
        delay = minuteElapsed >= std::chrono::minutes(1)
            ? std::chrono::milliseconds(1)
            : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::minutes(1) - minuteElapsed);
    }
    if (atOrder10sLimit) {
        const auto orderDelay = order10sElapsed >= std::chrono::seconds(10)
            ? std::chrono::milliseconds(1)
            : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds(10) - order10sElapsed);
        delay = (atWeightLimit || atOrderLimit) ? std::max(delay, orderDelay) : orderDelay;
    }
    timer.expires_after(delay);
    co_await timer.async_wait(boost::asio::use_awaitable);

    const auto resetAt = std::chrono::steady_clock::now();
    if (resetAt - m_windowStart.load() >= std::chrono::minutes(1)) {
        m_windowStart.store(resetAt);
        m_usedWeight = 0;
        m_usedOrders = 0;
    }
    if (resetAt - m_order10sWindowStart.load() >= std::chrono::seconds(10)) {
        m_order10sWindowStart.store(resetAt);
        m_usedOrders10s = 0;
    }
}
