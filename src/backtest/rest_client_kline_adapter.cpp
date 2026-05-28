/**
 * @file rest_client_kline_adapter.cpp
 * @brief REST kline pagination and timeout-aware adapter implementation.
 */

#include "backtest/rest_client_kline_adapter.h"

#include "rest/rest_client.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <optional>
#include <string>
#include <variant>

namespace backtest {

namespace detail {

std::optional<int64_t> parseIntervalMs(std::string_view interval) {
    if (interval.size() < 2) {
        return std::nullopt;
    }
    const char suffix = interval.back();
    int value = 0;
    const auto numberPart = interval.substr(0, interval.size() - 1);
    const auto* begin = numberPart.data();
    const auto* end = numberPart.data() + numberPart.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value <= 0) {
        return std::nullopt;
    }

    switch (suffix) {
        case 'm':
            return static_cast<int64_t>(value) * 60LL * 1000LL;
        case 'h':
            return static_cast<int64_t>(value) * 60LL * 60LL * 1000LL;
        case 'd':
            return static_cast<int64_t>(value) * 24LL * 60LL * 60LL * 1000LL;
        case 'w':
            return static_cast<int64_t>(value) * 7LL * 24LL * 60LL * 60LL * 1000LL;
        // 'M' (calendar month) intentionally rejected: variable 28-31 days, not a fixed ms.
        default:
            return std::nullopt;
    }
}

IKlineRestClient::FetchResult paginateClosedKlines(
    const PageFetcher& fetcher,
    std::string_view symbol,
    std::string_view interval,
    long long signalOpenMs,
    int limit,
    std::chrono::milliseconds timeout,
    int maxRequests) {

    IKlineRestClient::FetchResult out;
    const auto startWall = std::chrono::steady_clock::now();
    const auto elapsed = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startWall);
    };
    const auto setWallTime = [&]() { out.wallTime = elapsed(); };

    if (limit <= 0) {
        out.success = true;
        setWallTime();
        return out;
    }

    if (maxRequests <= 0) {
        out.errorMessage = "budget_exceeded need=1 max=0";
        setWallTime();
        return out;
    }

    const auto intervalMsOpt = parseIntervalMs(interval);
    if (!intervalMsOpt) {
        out.errorMessage = "unknown_interval";
        setWallTime();
        return out;
    }
    const int64_t intervalMs = *intervalMsOpt;

    const int pageSize = std::min(kBinanceKlinePageMax, limit);
    const int pagesNeeded = (limit + pageSize - 1) / pageSize;
    if (pagesNeeded > maxRequests) {
        out.errorMessage = "budget_exceeded need=" + std::to_string(pagesNeeded) +
            " max=" + std::to_string(maxRequests);
        setWallTime();
        return out;
    }

    std::vector<Kline> accumulated;
    accumulated.reserve(static_cast<size_t>(limit));

    // Off-by-one anchor: bar T closes at openTime + intervalMs - 1; this is the inclusive
    // endTime that Binance treats as "include bar T but not bar T+1".
    long long cursorEnd = signalOpenMs + intervalMs - 1LL;

    for (int page = 0; page < pagesNeeded; ++page) {
        const auto wallLeft = (timeout.count() > 0)
            ? timeout - elapsed()
            : std::chrono::milliseconds(0);
        if (timeout.count() > 0 && wallLeft <= std::chrono::milliseconds(0)) {
            out.pagesUsed = page;
            out.errorMessage = "timeout";
            setWallTime();
            return out;
        }

        const int remaining = limit - static_cast<int>(accumulated.size());
        if (remaining <= 0) {
            break;
        }
        const int thisPageLimit = std::min(pageSize, remaining);

        const PageOutcome page_result = fetcher(symbol, interval, thisPageLimit, cursorEnd, wallLeft);
        out.pagesUsed = page + 1;

        if (!page_result.ok) {
            if (page_result.error == "timeout") {
                out.errorMessage = "timeout";
            } else if (!page_result.error.empty()) {
                out.errorMessage = "rest_failed page=" + std::to_string(page + 1) +
                    " " + page_result.error;
            } else {
                out.errorMessage = "rest_failed page=" + std::to_string(page + 1);
            }
            setWallTime();
            return out;
        }
        if (page_result.bars.empty()) {
            out.errorMessage = "empty_page page=" + std::to_string(page + 1);
            setWallTime();
            return out;
        }

        accumulated.insert(accumulated.begin(), page_result.bars.begin(), page_result.bars.end());

        cursorEnd = page_result.bars.front().openTime - 1LL;
    }

    std::vector<Kline> filtered;
    filtered.reserve(accumulated.size());
    for (const auto& bar : accumulated) {
        if (!bar.isClosed) {
            continue;
        }
        if (bar.openTime > signalOpenMs) {
            continue;
        }
        filtered.push_back(bar);
    }

    std::sort(filtered.begin(), filtered.end(), [](const Kline& a, const Kline& b) {
        return a.openTime < b.openTime;
    });
    filtered.erase(
        std::unique(filtered.begin(), filtered.end(), [](const Kline& a, const Kline& b) {
            return a.openTime == b.openTime;
        }),
        filtered.end());

    if (static_cast<int>(filtered.size()) > limit) {
        filtered.erase(filtered.begin(), filtered.end() - limit);
    }

    out.success = true;
    out.bars = std::move(filtered);
    setWallTime();
    return out;
}

}  // namespace detail

namespace {

namespace asio = boost::asio;
using namespace boost::asio::experimental::awaitable_operators;

// Race RestClient::klines against a steady_timer. Returns std::nullopt on timeout (timer wins);
// otherwise propagates the REST call's Result. If `wallLeft <= 0` no timer is installed
// (caller controls cancellation through other means).
asio::awaitable<std::optional<Result<std::vector<Kline>>>> klinesWithTimeout(
    RestClient& rc,
    std::string symbol,
    std::string interval,
    int limit,
    std::optional<int64_t> endTime,
    std::chrono::milliseconds wallLeft) {

    if (wallLeft <= std::chrono::milliseconds(0)) {
        auto r = co_await rc.klines(std::move(symbol), std::move(interval), limit, std::nullopt, endTime);
        co_return std::optional<Result<std::vector<Kline>>>(std::move(r));
    }

    asio::steady_timer timer(co_await asio::this_coro::executor);
    timer.expires_after(wallLeft);

    auto race = co_await (
        rc.klines(std::move(symbol), std::move(interval), limit, std::nullopt, endTime) ||
        timer.async_wait(asio::use_awaitable));

    if (race.index() == 0) {
        co_return std::optional<Result<std::vector<Kline>>>(std::move(std::get<0>(race)));
    }
    co_return std::nullopt;
}

}  // namespace

RestClientKlineAdapter::RestClientKlineAdapter(RestClient& restClient, asio::io_context& ioc)
    : m_restClient(restClient), m_ioc(ioc) {}

IKlineRestClient::FetchResult RestClientKlineAdapter::fetchClosedKlines(
    std::string_view symbol,
    std::string_view interval,
    long long signalOpenMs,
    int limit,
    std::chrono::milliseconds timeout,
    int maxRequests) const {

    detail::PageFetcher fetcher = [this](std::string_view sym,
                                          std::string_view iv,
                                          int pageLimit,
                                          long long endTimeMs,
                                          std::chrono::milliseconds wallLeft) -> detail::PageOutcome {
        auto future = asio::co_spawn(
            m_ioc,
            klinesWithTimeout(m_restClient,
                              std::string(sym),
                              std::string(iv),
                              pageLimit,
                              std::optional<int64_t>(endTimeMs),
                              wallLeft),
            asio::use_future);

        detail::PageOutcome outcome;
        try {
            auto raceResult = future.get();
            if (!raceResult) {
                outcome.ok = false;
                outcome.error = "timeout";
                return outcome;
            }
            auto& restResult = *raceResult;
            if (!restResult) {
                outcome.ok = false;
                outcome.error = restResult.error().toString();
                return outcome;
            }
            outcome.ok = true;
            outcome.bars = std::move(*restResult);
            return outcome;
        } catch (const std::exception& e) {
            outcome.ok = false;
            outcome.error = std::string("exception: ") + e.what();
            return outcome;
        } catch (...) {
            outcome.ok = false;
            outcome.error = "exception: unknown";
            return outcome;
        }
    };

    return detail::paginateClosedKlines(fetcher, symbol, interval, signalOpenMs,
                                        limit, timeout, maxRequests);
}

}  // namespace backtest
