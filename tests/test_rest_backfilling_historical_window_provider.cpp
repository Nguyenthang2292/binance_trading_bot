#include "backtest/rest_backfilling_historical_window_provider.h"

#include "scanner/kline_cache.h"

#include <gtest/gtest.h>

#include <memory>

using namespace backtest;

namespace {

class StubInnerProvider : public IHistoricalWindowProvider {
public:
    WindowResult next{};
    mutable int calls{0};

    WindowResult closedWindow(
        std::string_view,
        std::string_view,
        int,
        std::chrono::system_clock::time_point) const override {
        ++calls;
        return next;
    }
};

class StubRestClient : public IKlineRestClient {
public:
    FetchResult next{};
    mutable int calls{0};
    mutable std::string lastSymbol{};
    mutable std::string lastInterval{};
    mutable long long lastSignalOpenMs{0};
    mutable int lastLimit{0};
    mutable std::chrono::milliseconds lastTimeout{0};
    mutable int lastMaxRequests{0};

    FetchResult fetchClosedKlines(
        std::string_view symbol,
        std::string_view interval,
        long long signalOpenMs,
        int limit,
        std::chrono::milliseconds timeout,
        int maxRequests) const override {
        ++calls;
        lastSymbol = std::string(symbol);
        lastInterval = std::string(interval);
        lastSignalOpenMs = signalOpenMs;
        lastLimit = limit;
        lastTimeout = timeout;
        lastMaxRequests = maxRequests;
        return next;
    }
};

Kline makeBar(int64_t openTimeMs, double price, bool closed = true) {
    Kline k{};
    k.openTime = openTimeMs;
    k.closeTime = openTimeMs + 60'000;
    k.open = k.close = price;
    k.high = price + 1.0;
    k.low = price - 1.0;
    k.isClosed = closed;
    return k;
}

std::chrono::system_clock::time_point tpFromMs(int64_t ms) {
    return std::chrono::system_clock::time_point{std::chrono::milliseconds(ms)};
}

}  // namespace

TEST(RestBackfillingHistoricalWindowProviderTest, ReturnsInnerResultWhenCacheIsSufficient) {
    scanner::KlineCache cache(200);

    auto inner = std::make_unique<StubInnerProvider>();
    inner->next.sufficient = true;
    inner->next.availableBars = 120;
    inner->next.requiredBars = 100;
    inner->next.closedKlines = {makeBar(0, 100.0), makeBar(60'000, 101.0)};
    auto* innerRaw = inner.get();

    auto rest = std::make_unique<StubRestClient>();
    auto* restRaw = rest.get();

    BacktestGateDataConfig cfg;
    cfg.runtimeRestFetchEnabled = true;

    RestBackfillingHistoricalWindowProvider provider(
        std::move(inner), std::move(rest), cache, cfg);

    const auto result = provider.closedWindow("BTCUSDT", "1m", 2, tpFromMs(60'000));
    EXPECT_TRUE(result.sufficient);
    EXPECT_EQ(result.source, "cache");
    EXPECT_EQ(innerRaw->calls, 1);
    EXPECT_EQ(restRaw->calls, 0);
}

TEST(RestBackfillingHistoricalWindowProviderTest, BackfillsFromRestAndWritesBackCache) {
    scanner::KlineCache cache(300);

    auto inner = std::make_unique<StubInnerProvider>();
    inner->next.sufficient = false;
    inner->next.availableBars = 10;
    inner->next.requiredBars = 50;
    auto* innerRaw = inner.get();

    auto rest = std::make_unique<StubRestClient>();
    constexpr int requiredBars = 50;
    constexpr int64_t signalMs = 49LL * 60'000;
    for (int i = 0; i < requiredBars; ++i) {
        rest->next.bars.push_back(makeBar(i * 60'000LL, 100.0 + static_cast<double>(i)));
    }
    rest->next.success = true;
    rest->next.pagesUsed = 1;
    rest->next.wallTime = std::chrono::milliseconds(37);
    auto* restRaw = rest.get();

    BacktestGateDataConfig cfg;
    cfg.runtimeRestFetchEnabled = true;
    cfg.runtimeRestFetchTimeoutSeconds = 10;
    cfg.maxRestRequestsPerSignal = 3;

    RestBackfillingHistoricalWindowProvider provider(
        std::move(inner), std::move(rest), cache, cfg);

    const auto result = provider.closedWindow("BTCUSDT", "1m", requiredBars, tpFromMs(signalMs));
    ASSERT_TRUE(result.sufficient);
    EXPECT_EQ(result.source, "rest");
    EXPECT_EQ(result.restPagesUsed, 1);
    EXPECT_EQ(result.restWallTimeMs.count(), 37);
    ASSERT_EQ(result.closedKlines.size(), static_cast<size_t>(requiredBars));
    EXPECT_EQ(result.closedKlines.back().openTime, signalMs);
    EXPECT_EQ(result.errorReason, "");

    EXPECT_EQ(innerRaw->calls, 1);
    EXPECT_EQ(restRaw->calls, 1);
    EXPECT_EQ(restRaw->lastSymbol, "BTCUSDT");
    EXPECT_EQ(restRaw->lastInterval, "1m");
    EXPECT_EQ(restRaw->lastLimit, requiredBars);
    EXPECT_EQ(restRaw->lastSignalOpenMs, signalMs);

    const auto cached = cache.snapshot("BTCUSDT", "1m");
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(cached->back().openTime, signalMs);
}

TEST(RestBackfillingHistoricalWindowProviderTest, ReturnsInsufficientWhenRestFails) {
    scanner::KlineCache cache(200);

    auto inner = std::make_unique<StubInnerProvider>();
    inner->next.sufficient = false;
    inner->next.availableBars = 7;

    auto rest = std::make_unique<StubRestClient>();
    rest->next.success = false;
    rest->next.errorMessage = "timeout";
    rest->next.pagesUsed = 1;
    rest->next.wallTime = std::chrono::milliseconds(1000);

    BacktestGateDataConfig cfg;
    cfg.runtimeRestFetchEnabled = true;

    RestBackfillingHistoricalWindowProvider provider(
        std::move(inner), std::move(rest), cache, cfg);

    const auto result = provider.closedWindow("BTCUSDT", "1m", 50, tpFromMs(49LL * 60'000));
    EXPECT_FALSE(result.sufficient);
    EXPECT_EQ(result.source, "rest");
    EXPECT_EQ(result.errorReason, "timeout");
    EXPECT_EQ(result.restPagesUsed, 1);
    EXPECT_EQ(result.availableBars, 7);
}

TEST(RestBackfillingHistoricalWindowProviderTest, ReturnsInsufficientHistoryWhenRestHasTooFewBars) {
    scanner::KlineCache cache(200);

    auto inner = std::make_unique<StubInnerProvider>();
    inner->next.sufficient = false;
    inner->next.availableBars = 2;

    auto rest = std::make_unique<StubRestClient>();
    rest->next.success = true;
    rest->next.bars = {makeBar(0, 100.0), makeBar(60'000, 101.0)};
    rest->next.pagesUsed = 1;
    rest->next.wallTime = std::chrono::milliseconds(42);

    BacktestGateDataConfig cfg;
    cfg.runtimeRestFetchEnabled = true;

    RestBackfillingHistoricalWindowProvider provider(
        std::move(inner), std::move(rest), cache, cfg);

    const auto result = provider.closedWindow("BTCUSDT", "1m", 3, tpFromMs(60'000));
    EXPECT_FALSE(result.sufficient);
    EXPECT_EQ(result.source, "rest");
    EXPECT_EQ(result.errorReason, "insufficient_history");
    EXPECT_EQ(result.availableBars, 2);
    EXPECT_EQ(result.restPagesUsed, 1);
}

TEST(RestBackfillingHistoricalWindowProviderTest, ReturnsSignalBarMissingWhenRestWindowInvalid) {
    scanner::KlineCache cache(200);

    auto inner = std::make_unique<StubInnerProvider>();
    inner->next.sufficient = false;

    auto rest = std::make_unique<StubRestClient>();
    rest->next.success = true;
    rest->next.bars = {makeBar(0, 100.0), makeBar(60'000, 101.0)};
    // Ask for signal at 120000, but response ends at 60000 -> invalid.

    BacktestGateDataConfig cfg;
    cfg.runtimeRestFetchEnabled = true;

    RestBackfillingHistoricalWindowProvider provider(
        std::move(inner), std::move(rest), cache, cfg);

    const auto result = provider.closedWindow("BTCUSDT", "1m", 2, tpFromMs(120'000));
    EXPECT_FALSE(result.sufficient);
    EXPECT_EQ(result.source, "rest");
    EXPECT_EQ(result.errorReason, "signal_bar_missing");
}
