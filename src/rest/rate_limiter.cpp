#include "rest/rate_limiter.h"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <cmath>

RateLimiter::RateLimiter() : RateLimiter(Limits{}) {}

RateLimiter::RateLimiter(Limits limits)
    : m_limits(limits),
      m_windowStart(std::chrono::steady_clock::now()),
      m_order10sWindowStart(std::chrono::steady_clock::now()),
      m_penaltyUntil(std::chrono::steady_clock::time_point::min()) {}

int RateLimiter::klineWeight(int limit) {
    if (limit < 1) {
        return 1;
    }
    if (limit < 100) {
        return 1;
    }
    if (limit < 500) {
        return 2;
    }
    if (limit <= 1000) {
        return 5;
    }
    return 10;
}

int RateLimiter::safeBudget(int limit, double safetyRatio) {
    if (limit <= 0) {
        return 0;
    }
    const double ratio = std::clamp(safetyRatio, 0.1, 1.0);
    return std::max(1, static_cast<int>(std::floor(static_cast<double>(limit) * ratio)));
}

void RateLimiter::refreshWindowsLocked(std::chrono::steady_clock::time_point now) {
    if (now - m_windowStart >= std::chrono::minutes(1)) {
        m_windowStart = now;
        m_headerUsedWeight = 0;
        m_headerUsedOrders1m = 0;
        m_reservedWeight = 0;
        m_reservedOrders1m = 0;
    }
    if (now - m_order10sWindowStart >= std::chrono::seconds(10)) {
        m_order10sWindowStart = now;
        m_headerUsedOrders10s = 0;
        m_reservedOrders10s = 0;
    }
}

boost::asio::awaitable<void> RateLimiter::acquire(Cost cost) {
    if (cost.requestWeight < 0) {
        cost.requestWeight = 0;
    }
    if (cost.orders1m < 0) {
        cost.orders1m = 0;
    }
    if (cost.orders10s < 0) {
        cost.orders10s = 0;
    }

    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor);
    while (true) {
        std::chrono::milliseconds delay{1};
        bool granted = false;
        {
            std::scoped_lock lock(m_mutex);
            const auto now = std::chrono::steady_clock::now();
            refreshWindowsLocked(now);

            if (now < m_penaltyUntil) {
                delay = std::chrono::duration_cast<std::chrono::milliseconds>(m_penaltyUntil - now);
            } else {
                const int effectiveWeight = std::max(m_headerUsedWeight, m_reservedWeight);
                const int effectiveOrders1m = std::max(m_headerUsedOrders1m, m_reservedOrders1m);
                const int effectiveOrders10s = std::max(m_headerUsedOrders10s, m_reservedOrders10s);

                const int weightBudget = safeBudget(m_limits.requestWeightPerMinute, m_limits.safetyRatio);
                const int orders1mBudget = safeBudget(m_limits.ordersPerMinute, m_limits.safetyRatio);
                const int orders10sBudget = safeBudget(m_limits.ordersPer10Seconds, m_limits.safetyRatio);

                const bool weightOk = (effectiveWeight + cost.requestWeight) <= weightBudget;
                const bool order1mOk = (effectiveOrders1m + cost.orders1m) <= orders1mBudget;
                const bool order10sOk = (effectiveOrders10s + cost.orders10s) <= orders10sBudget;

                if (weightOk && order1mOk && order10sOk) {
                    m_reservedWeight += cost.requestWeight;
                    m_reservedOrders1m += cost.orders1m;
                    m_reservedOrders10s += cost.orders10s;
                    granted = true;
                } else {
                    const auto minuteRemaining = now - m_windowStart >= std::chrono::minutes(1)
                        ? std::chrono::milliseconds(1)
                        : std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::minutes(1) - (now - m_windowStart));
                    const auto tenSecRemaining = now - m_order10sWindowStart >= std::chrono::seconds(10)
                        ? std::chrono::milliseconds(1)
                        : std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::seconds(10) - (now - m_order10sWindowStart));

                    delay = std::chrono::milliseconds(1);
                    if (!weightOk || !order1mOk) {
                        delay = std::max(delay, minuteRemaining);
                    }
                    if (!order10sOk) {
                        delay = std::max(delay, tenSecRemaining);
                    }
                }
            }
        }

        if (granted) {
            co_return;
        }

        timer.expires_after(delay);
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
}

void RateLimiter::updateFromHeaders(int usedWeight, int usedOrders1m, int usedOrders10s) {
    std::scoped_lock lock(m_mutex);
    const auto now = std::chrono::steady_clock::now();
    refreshWindowsLocked(now);

    if (usedWeight >= 0) {
        m_headerUsedWeight = usedWeight;
    }
    if (usedOrders1m >= 0) {
        m_headerUsedOrders1m = usedOrders1m;
    }
    if (usedOrders10s >= 0) {
        m_headerUsedOrders10s = usedOrders10s;
    }
}

int RateLimiter::depthWeight(int limit) {
    if (limit <= 50) {
        return 2;
    }
    if (limit <= 100) {
        return 5;
    }
    if (limit <= 500) {
        return 10;
    }
    return 20;
}

void RateLimiter::release(Cost cost) {
    if (cost.requestWeight < 0) {
        cost.requestWeight = 0;
    }
    if (cost.orders1m < 0) {
        cost.orders1m = 0;
    }
    if (cost.orders10s < 0) {
        cost.orders10s = 0;
    }

    std::scoped_lock lock(m_mutex);
    m_reservedWeight = std::max(0, m_reservedWeight - cost.requestWeight);
    m_reservedOrders1m = std::max(0, m_reservedOrders1m - cost.orders1m);
    m_reservedOrders10s = std::max(0, m_reservedOrders10s - cost.orders10s);
}

void RateLimiter::penalize(std::chrono::milliseconds delay) {
    if (delay.count() <= 0) {
        return;
    }
    std::scoped_lock lock(m_mutex);
    const auto until = std::chrono::steady_clock::now() + delay;
    if (until > m_penaltyUntil) {
        m_penaltyUntil = until;
    }
}

void RateLimiter::update(int usedWeight, int usedOrders) {
    update(usedWeight, usedOrders, -1);
}

void RateLimiter::update(int usedWeight, int usedOrders, int usedOrders10s) {
    updateFromHeaders(usedWeight, usedOrders, usedOrders10s);
}

bool RateLimiter::isNearLimit() const {
    std::scoped_lock lock(m_mutex);
    const int effectiveWeight = std::max(m_headerUsedWeight, m_reservedWeight);
    const int effectiveOrders1m = std::max(m_headerUsedOrders1m, m_reservedOrders1m);
    const int effectiveOrders10s = std::max(m_headerUsedOrders10s, m_reservedOrders10s);
    return effectiveWeight >= static_cast<int>(m_limits.requestWeightPerMinute * 0.8) ||
           effectiveOrders1m >= static_cast<int>(m_limits.ordersPerMinute * 0.8) ||
           effectiveOrders10s >= static_cast<int>(m_limits.ordersPer10Seconds * 0.8);
}

boost::asio::awaitable<void> RateLimiter::waitIfNeeded() {
    co_await acquire(Cost{});
}
