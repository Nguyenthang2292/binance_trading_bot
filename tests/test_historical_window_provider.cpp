#include "backtest/historical_window_provider.h"
#include "scanner/kline_cache.h"

#include <gtest/gtest.h>

using namespace backtest;

namespace {

Kline makeBar(int64_t openTimeMs, double price, bool closed = true) {
    Kline k{};
    k.openTime = openTimeMs;
    k.closeTime = openTimeMs + 60'000;
    k.open = k.close = price;
    k.high = price + 1.0;
    k.low  = price - 1.0;
    k.isClosed = closed;
    return k;
}

}  // namespace

TEST(HistoricalWindowProviderTest, ReportsInsufficientWhenCacheEmpty) {
    scanner::KlineCache cache(/*bufferSize=*/200);
    BacktestGateDataConfig cfg;
    HistoricalWindowProvider p(cache, cfg);

    auto r = p.closedWindow("BTCUSDT", "1h", 100,
        std::chrono::system_clock::from_time_t(1'700'000'000));
    EXPECT_FALSE(r.sufficient);
    EXPECT_EQ(r.availableBars, 0);
    EXPECT_EQ(r.requiredBars, 100);
}

TEST(HistoricalWindowProviderTest, SufficientWhenCacheHasEnoughClosed) {
    scanner::KlineCache cache(/*bufferSize=*/300);
    for (int i = 0; i < 150; ++i) {
        cache.update("BTCUSDT", "1h", makeBar(i * 60'000, 100.0));
    }
    BacktestGateDataConfig cfg;
    HistoricalWindowProvider p(cache, cfg);

    // Request signalBarOpenTime == last cached bar openTime (149 * 60_000).
    const int64_t signalMs = 149LL * 60'000;
    auto signalTp = std::chrono::system_clock::from_time_t(0) +
                    std::chrono::milliseconds(signalMs);

    auto r = p.closedWindow("BTCUSDT", "1h", 100, signalTp);
    EXPECT_TRUE(r.sufficient);
    EXPECT_GE(r.availableBars, 100);
    EXPECT_EQ(r.closedKlines.size(), 100u);
    EXPECT_EQ(r.closedKlines.back().openTime, signalMs);
}

TEST(HistoricalWindowProviderTest, ExcludesOpenBars) {
    scanner::KlineCache cache(/*bufferSize=*/300);
    for (int i = 0; i < 100; ++i) {
        cache.update("BTCUSDT", "1h", makeBar(i * 60'000, 100.0, /*closed=*/true));
    }
    // Add one open (forming) bar.
    cache.update("BTCUSDT", "1h", makeBar(100 * 60'000, 100.0, /*closed=*/false));

    BacktestGateDataConfig cfg;
    HistoricalWindowProvider p(cache, cfg);

    const int64_t signalMs = 99LL * 60'000;
    auto signalTp = std::chrono::system_clock::from_time_t(0) +
                    std::chrono::milliseconds(signalMs);

    auto r = p.closedWindow("BTCUSDT", "1h", 50, signalTp);
    ASSERT_TRUE(r.sufficient);
    // Last returned bar is the closed signal bar at index 99, NOT the open bar at 100.
    EXPECT_EQ(r.closedKlines.back().openTime, signalMs);
    for (const auto& k : r.closedKlines) {
        EXPECT_TRUE(k.isClosed);
    }
}

TEST(HistoricalWindowProviderTest, RejectsWhenSignalBarMissing) {
    // Cache only has bars up to t=99, but we ask for signal at t=200.
    scanner::KlineCache cache(/*bufferSize=*/300);
    for (int i = 0; i < 100; ++i) {
        cache.update("BTCUSDT", "1h", makeBar(i * 60'000, 100.0));
    }
    BacktestGateDataConfig cfg;
    HistoricalWindowProvider p(cache, cfg);

    auto signalTp = std::chrono::system_clock::from_time_t(0) +
                    std::chrono::milliseconds(200LL * 60'000);

    auto r = p.closedWindow("BTCUSDT", "1h", 50, signalTp);
    EXPECT_FALSE(r.sufficient);
}
