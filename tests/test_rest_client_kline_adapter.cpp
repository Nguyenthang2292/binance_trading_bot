#include "backtest/rest_client_kline_adapter.h"
#include "context.h"
#include "rest/rest_client.h"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace backtest;
using backtest::detail::PageFetcher;
using backtest::detail::PageOutcome;
using backtest::detail::paginateClosedKlines;
using backtest::detail::parseIntervalMs;

namespace {

constexpr int64_t k1mMs = 60LL * 1000LL;
constexpr int64_t k30mMs = 30LL * 60LL * 1000LL;

Kline makeBar(int64_t openTimeMs, int64_t intervalMs, double price, bool closed = true) {
    Kline k{};
    k.openTime = openTimeMs;
    k.closeTime = openTimeMs + intervalMs - 1;
    k.open = k.close = price;
    k.high = price + 1.0;
    k.low = price - 1.0;
    k.isClosed = closed;
    return k;
}

struct CallRecord {
    std::string symbol;
    std::string interval;
    int limit{0};
    long long endTimeMs{0};
    std::chrono::milliseconds timeout{0};
};

// Fake fetcher that returns a contiguous slice of the canned `inventory` ending exactly at
// `endTimeMs` (inclusive of the bar whose close-time matches the cursor). Records every call.
class FakeFetcher {
public:
    explicit FakeFetcher(std::vector<Kline> inventory, int64_t intervalMs)
        : m_inventory(std::move(inventory)), m_intervalMs(intervalMs) {}

    PageFetcher asCallback() {
        return [this](std::string_view sym, std::string_view iv, int limit,
                      long long endTimeMs, std::chrono::milliseconds timeout) {
            calls.push_back(CallRecord{
                std::string(sym), std::string(iv), limit, endTimeMs, timeout});
            return serve(limit, endTimeMs);
        };
    }

    std::vector<CallRecord> calls;

private:
    PageOutcome serve(int limit, long long endTimeMs) const {
        PageOutcome out;
        // Binance semantics: include bar T where openTime + intervalMs - 1 <= endTimeMs.
        std::vector<Kline> slice;
        for (const auto& bar : m_inventory) {
            if (bar.openTime + m_intervalMs - 1 <= endTimeMs) {
                slice.push_back(bar);
            }
        }
        // Take last `limit` after filtering; result must be ascending (Binance native).
        if (static_cast<int>(slice.size()) > limit) {
            slice.erase(slice.begin(), slice.end() - limit);
        }
        out.ok = true;
        out.bars = std::move(slice);
        return out;
    }

    std::vector<Kline> m_inventory;
    int64_t m_intervalMs;
};

std::vector<Kline> makeContiguousBars(int64_t startOpenMs, int64_t intervalMs, int count) {
    std::vector<Kline> bars;
    bars.reserve(count);
    for (int i = 0; i < count; ++i) {
        bars.push_back(makeBar(startOpenMs + i * intervalMs, intervalMs, 100.0 + i));
    }
    return bars;
}

}  // namespace

TEST(ParseIntervalMsTest, AcceptsMinuteHourDayWeek) {
    EXPECT_EQ(parseIntervalMs("1m").value_or(-1), 60LL * 1000LL);
    EXPECT_EQ(parseIntervalMs("30m").value_or(-1), 30LL * 60LL * 1000LL);
    EXPECT_EQ(parseIntervalMs("4h").value_or(-1), 4LL * 60LL * 60LL * 1000LL);
    EXPECT_EQ(parseIntervalMs("1d").value_or(-1), 24LL * 60LL * 60LL * 1000LL);
    EXPECT_EQ(parseIntervalMs("1w").value_or(-1), 7LL * 24LL * 60LL * 60LL * 1000LL);
}

TEST(ParseIntervalMsTest, RejectsCalendarMonthAndInvalid) {
    EXPECT_FALSE(parseIntervalMs("1M").has_value());     // calendar month rejected
    EXPECT_FALSE(parseIntervalMs("3M").has_value());
    EXPECT_FALSE(parseIntervalMs("0m").has_value());     // zero rejected
    EXPECT_FALSE(parseIntervalMs("-1m").has_value());    // negative rejected
    EXPECT_FALSE(parseIntervalMs("m").has_value());      // missing number
    EXPECT_FALSE(parseIntervalMs("1s").has_value());     // unknown suffix
    EXPECT_FALSE(parseIntervalMs("").has_value());
}

TEST(PaginateClosedKlinesTest, SinglePageReturnsBarsAndAnchorIsCloseTimeOfSignalBar) {
    // Window of 10 bars at 1m ending at signal openTime = 100*60000 (bar #100).
    const int64_t signalOpen = 100 * k1mMs;
    auto bars = makeContiguousBars((100 - 9) * k1mMs, k1mMs, 10);
    FakeFetcher fake(bars, k1mMs);

    auto result = paginateClosedKlines(
        fake.asCallback(), "BTCUSDT", "1m", signalOpen, 10,
        std::chrono::milliseconds(5000), 3);

    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(result.pagesUsed, 1);
    ASSERT_EQ(result.bars.size(), 10u);
    EXPECT_EQ(result.bars.front().openTime, (100 - 9) * k1mMs);
    EXPECT_EQ(result.bars.back().openTime, signalOpen);

    // Off-by-one anchor: endTime passed to fetcher MUST be signalOpen + intervalMs - 1.
    ASSERT_EQ(fake.calls.size(), 1u);
    EXPECT_EQ(fake.calls[0].endTimeMs, signalOpen + k1mMs - 1);
    EXPECT_EQ(fake.calls[0].limit, 10);
    EXPECT_EQ(fake.calls[0].symbol, "BTCUSDT");
    EXPECT_EQ(fake.calls[0].interval, "1m");
}

TEST(PaginateClosedKlinesTest, FifteenHundredFitsInOneRequest) {
    const int64_t signalOpen = 5000 * k1mMs;
    auto bars = makeContiguousBars((5000 - 1499) * k1mMs, k1mMs, 1500);
    FakeFetcher fake(bars, k1mMs);

    auto result = paginateClosedKlines(
        fake.asCallback(), "BTCUSDT", "1m", signalOpen, 1500,
        std::chrono::milliseconds(10000), 3);

    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(result.pagesUsed, 1);
    EXPECT_EQ(result.bars.size(), 1500u);
    EXPECT_EQ(fake.calls.size(), 1u);
}

TEST(PaginateClosedKlinesTest, ThreeThousandPaginatesIntoTwoCalls) {
    const int64_t signalOpen = 10000 * k1mMs;
    auto bars = makeContiguousBars((10000 - 2999) * k1mMs, k1mMs, 3000);
    FakeFetcher fake(bars, k1mMs);

    auto result = paginateClosedKlines(
        fake.asCallback(), "BTCUSDT", "1m", signalOpen, 3000,
        std::chrono::milliseconds(10000), 3);

    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(result.pagesUsed, 2);
    EXPECT_EQ(result.bars.size(), 3000u);
    EXPECT_EQ(result.bars.front().openTime, (10000 - 2999) * k1mMs);
    EXPECT_EQ(result.bars.back().openTime, signalOpen);

    ASSERT_EQ(fake.calls.size(), 2u);
    // First call anchored at signal bar close-time.
    EXPECT_EQ(fake.calls[0].endTimeMs, signalOpen + k1mMs - 1);
    EXPECT_EQ(fake.calls[0].limit, 1500);
    // Second call: endTime = (oldest from first page).openTime - 1
    // Oldest from first page = signalOpen - 1499 * k1mMs.
    EXPECT_EQ(fake.calls[1].endTimeMs, (signalOpen - 1499 * k1mMs) - 1);
    EXPECT_EQ(fake.calls[1].limit, 1500);
}

TEST(PaginateClosedKlinesTest, FailsBudgetBeforeAnyCallWhenLimitTooLarge) {
    FakeFetcher fake({}, k1mMs);

    auto result = paginateClosedKlines(
        fake.asCallback(), "BTCUSDT", "1m", 1000 * k1mMs, 5000,
        std::chrono::milliseconds(10000), 3);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage.rfind("budget_exceeded", 0), 0u);
    EXPECT_TRUE(fake.calls.empty());
    EXPECT_EQ(result.pagesUsed, 0);
}

TEST(PaginateClosedKlinesTest, FailsOnUnknownInterval) {
    FakeFetcher fake({}, k1mMs);

    auto result = paginateClosedKlines(
        fake.asCallback(), "BTCUSDT", "1M", 0, 10,
        std::chrono::milliseconds(10000), 3);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "unknown_interval");
    EXPECT_TRUE(fake.calls.empty());
}

TEST(PaginateClosedKlinesTest, SecondPageErrorIsPropagatedAsRestFailed) {
    int callCount = 0;
    PageFetcher fetcher = [&](std::string_view, std::string_view, int limit,
                              long long, std::chrono::milliseconds) -> PageOutcome {
        ++callCount;
        PageOutcome out;
        if (callCount == 1) {
            out.ok = true;
            out.bars = makeContiguousBars(0, k1mMs, limit);
        } else {
            out.ok = false;
            out.error = "http_502";
        }
        return out;
    };

    auto result = paginateClosedKlines(
        fetcher, "BTCUSDT", "1m", 2999 * k1mMs, 3000,
        std::chrono::milliseconds(10000), 3);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.pagesUsed, 2);
    EXPECT_NE(result.errorMessage.find("rest_failed page=2"), std::string::npos);
    EXPECT_NE(result.errorMessage.find("http_502"), std::string::npos);
    EXPECT_TRUE(result.bars.empty());  // no partial data leaks
}

TEST(PaginateClosedKlinesTest, FetcherTimeoutIsSurfacedAsTimeout) {
    PageFetcher fetcher = [](std::string_view, std::string_view, int,
                             long long, std::chrono::milliseconds) -> PageOutcome {
        PageOutcome out;
        out.ok = false;
        out.error = "timeout";
        return out;
    };

    auto result = paginateClosedKlines(
        fetcher, "BTCUSDT", "1m", 1000 * k1mMs, 100,
        std::chrono::milliseconds(50), 3);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "timeout");
    EXPECT_EQ(result.pagesUsed, 1);
}

TEST(PaginateClosedKlinesTest, EmptyPageIsTreatedAsFailure) {
    PageFetcher fetcher = [](std::string_view, std::string_view, int,
                             long long, std::chrono::milliseconds) -> PageOutcome {
        PageOutcome out;
        out.ok = true;
        out.bars = {};  // empty
        return out;
    };

    auto result = paginateClosedKlines(
        fetcher, "BTCUSDT", "1m", 1000 * k1mMs, 50,
        std::chrono::milliseconds(5000), 3);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("empty_page"), std::string::npos);
    EXPECT_TRUE(result.bars.empty());
}

TEST(PaginateClosedKlinesTest, FiltersOutNonClosedAndPostSignalBars) {
    const int64_t signalOpen = 50 * k1mMs;
    // 50 closed bars at openTimes 1..50 (× k1mMs).
    auto closedBars = makeContiguousBars(k1mMs, k1mMs, 50);

    PageFetcher fetcher = [&](std::string_view, std::string_view, int /*limit*/,
                              long long, std::chrono::milliseconds) -> PageOutcome {
        PageOutcome out;
        out.ok = true;
        out.bars = closedBars;
        // Stray in-progress future bar (Binance can include the in-progress bar at endTime edge).
        Kline futureBar = makeBar(signalOpen + k1mMs, k1mMs, 999.0, /*closed=*/false);
        out.bars.push_back(futureBar);
        // Also a non-closed bar AT signalOpen masquerading as the signal bar; should be dropped.
        Kline halfOpenSignal = makeBar(signalOpen, k1mMs, 888.0, /*closed=*/false);
        halfOpenSignal.openTime = signalOpen;  // duplicate openTime, but isClosed=false
        out.bars.push_back(halfOpenSignal);
        return out;
    };

    auto result = paginateClosedKlines(
        fetcher, "BTCUSDT", "1m", signalOpen, 50,
        std::chrono::milliseconds(5000), 3);

    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(result.bars.size(), 50u);
    EXPECT_EQ(result.bars.back().openTime, signalOpen);
    EXPECT_TRUE(result.bars.back().isClosed);
    for (const auto& bar : result.bars) {
        EXPECT_TRUE(bar.isClosed);
        EXPECT_LE(bar.openTime, signalOpen);
    }
}

TEST(PaginateClosedKlinesTest, MaxRequestsZeroFailsImmediately) {
    int calls = 0;
    PageFetcher fetcher = [&](std::string_view, std::string_view, int, long long,
                              std::chrono::milliseconds) {
        ++calls;
        return PageOutcome{};
    };

    auto result = paginateClosedKlines(
        fetcher, "BTCUSDT", "1m", 0, 100,
        std::chrono::milliseconds(5000), 0);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage.rfind("budget_exceeded", 0), 0u);
    EXPECT_EQ(calls, 0);
}

TEST(PaginateClosedKlinesTest, ZeroLimitReturnsSuccessWithoutCallingFetcher) {
    int calls = 0;
    PageFetcher fetcher = [&](std::string_view, std::string_view, int, long long,
                              std::chrono::milliseconds) {
        ++calls;
        return PageOutcome{};
    };

    auto result = paginateClosedKlines(
        fetcher, "BTCUSDT", "1m", 0, 0,
        std::chrono::milliseconds(5000), 3);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.bars.empty());
    EXPECT_EQ(calls, 0);
}

TEST(PaginateClosedKlinesTest, OffByOneAnchorWith30mInterval) {
    const int64_t signalOpen = 1'700'000'000'000LL;  // arbitrary
    auto bars = makeContiguousBars(signalOpen - 9 * k30mMs, k30mMs, 10);
    FakeFetcher fake(bars, k30mMs);

    auto result = paginateClosedKlines(
        fake.asCallback(), "BTCUSDT", "30m", signalOpen, 10,
        std::chrono::milliseconds(5000), 3);

    ASSERT_TRUE(result.success) << result.errorMessage;
    ASSERT_EQ(fake.calls.size(), 1u);
    EXPECT_EQ(fake.calls[0].endTimeMs, signalOpen + k30mMs - 1);
    EXPECT_EQ(result.bars.back().openTime, signalOpen);
}

TEST(PaginateClosedKlinesTest, RemainingWallTimeShrinksAcrossPages) {
    // Two pages; each fetcher call sleeps logically by reporting how much budget arrived.
    std::vector<std::chrono::milliseconds> reportedTimeouts;
    int call = 0;
    PageFetcher fetcher = [&](std::string_view, std::string_view, int limit,
                              long long, std::chrono::milliseconds wallLeft) -> PageOutcome {
        reportedTimeouts.push_back(wallLeft);
        ++call;
        PageOutcome out;
        out.ok = true;
        out.bars = makeContiguousBars((call - 1) * k1mMs * limit, k1mMs, limit);
        // Consume time between pages so the second receives less budget.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return out;
    };

    auto result = paginateClosedKlines(
        fetcher, "BTCUSDT", "1m", 2999 * k1mMs, 3000,
        std::chrono::milliseconds(500), 3);

    ASSERT_EQ(reportedTimeouts.size(), 2u);
    EXPECT_GT(reportedTimeouts[0].count(), reportedTimeouts[1].count());
    EXPECT_LE(reportedTimeouts[0].count(), 500);
    EXPECT_GE(reportedTimeouts[1].count(), 0);
    EXPECT_LT(reportedTimeouts[1].count(), 500);
    (void)result;
}

TEST(RestClientKlineAdapterTest, FailsClosedWhenCalledFromIoContextThread) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl(boost::asio::ssl::context::tls_client);
    ContextConfig cfg;
    cfg.apiKey = "";
    cfg.secretKey = "";
    cfg.testnet = true;
    RestClient rest(ioc, ssl, cfg);
    RestClientKlineAdapter adapter(rest, ioc);

    auto task = [&]() -> boost::asio::awaitable<IKlineRestClient::FetchResult> {
        co_return adapter.fetchClosedKlines(
            "BTCUSDT",
            "1m",
            1'000'000,
            5,
            std::chrono::milliseconds(100),
            1);
    };

    auto fut = boost::asio::co_spawn(ioc, task(), boost::asio::use_future);
    ioc.run();
    auto result = fut.get();
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "rest_failed page=1 io_thread_blocking_forbidden");
}
