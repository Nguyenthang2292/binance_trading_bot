// Integration test for `RestClientKlineAdapter` against the real Binance USD-M Futures REST API.
//
// Gated by env var: set BINANCE_INTEGRATION_TEST=1 to run, otherwise every test self-skips.
// Run with: BINANCE_INTEGRATION_TEST=1 ctest -R RestClientKlineAdapterIntegration -V
//
// MSVC multi-config note: this test compiles into the normal test binary; the env-var gate
// (GTEST_SKIP) is what excludes it from default ctest runs — no CMake `CONFIGURATIONS` label
// is used because the Visual Studio generator does not support custom config names.

#include "backtest/rest_client_kline_adapter.h"
#include "context.h"
#include "rest/rest_client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>

namespace {

bool integrationEnabled() {
    const char* flag = std::getenv("BINANCE_INTEGRATION_TEST");
    return flag != nullptr && std::string_view(flag) == "1";
}

ContextConfig publicReadOnlyConfig() {
    ContextConfig cfg;
    cfg.apiKey = "";
    cfg.secretKey = "";
    cfg.testnet = false;
    cfg.threadPoolSize = 2;
    return cfg;
}

constexpr int64_t k30mMs = 30LL * 60LL * 1000LL;

}  // namespace

TEST(RestClientKlineAdapterIntegration, FetchBtcUsdt30mRecentSmallWindow) {
    if (!integrationEnabled()) {
        GTEST_SKIP() << "Set BINANCE_INTEGRATION_TEST=1 to run live network test.";
    }

    BinanceContext ctx(publicReadOnlyConfig());
    RestClient rest = ctx.makeRestClient();
    backtest::RestClientKlineAdapter adapter(rest, ctx.ioc());

    // Anchor on the most recent fully-closed 30m bar.
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t lastClosedOpen = (nowMs / k30mMs) * k30mMs - k30mMs;

    auto result = adapter.fetchClosedKlines(
        "BTCUSDT", "30m", lastClosedOpen, 10,
        std::chrono::milliseconds(10000), 3);

    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(result.pagesUsed, 1);
    ASSERT_EQ(result.bars.size(), 10u);
    EXPECT_EQ(result.bars.back().openTime, lastClosedOpen);

    // Verify monotonic 30m stride.
    for (size_t i = 1; i < result.bars.size(); ++i) {
        EXPECT_EQ(result.bars[i].openTime - result.bars[i - 1].openTime, k30mMs);
        EXPECT_TRUE(result.bars[i].isClosed);
    }
}

TEST(RestClientKlineAdapterIntegration, FetchBtcUsdt30mPaginatesTo3000) {
    if (!integrationEnabled()) {
        GTEST_SKIP() << "Set BINANCE_INTEGRATION_TEST=1 to run live network test.";
    }

    BinanceContext ctx(publicReadOnlyConfig());
    RestClient rest = ctx.makeRestClient();
    backtest::RestClientKlineAdapter adapter(rest, ctx.ioc());

    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t lastClosedOpen = (nowMs / k30mMs) * k30mMs - k30mMs;

    auto result = adapter.fetchClosedKlines(
        "BTCUSDT", "30m", lastClosedOpen, 3000,
        std::chrono::milliseconds(30000), 3);

    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(result.pagesUsed, 2);
    EXPECT_EQ(result.bars.size(), 3000u);
    EXPECT_EQ(result.bars.back().openTime, lastClosedOpen);

    // No gaps: every consecutive pair must differ by exactly one interval.
    for (size_t i = 1; i < result.bars.size(); ++i) {
        ASSERT_EQ(result.bars[i].openTime - result.bars[i - 1].openTime, k30mMs)
            << "gap at index " << i;
    }
}

TEST(RestClientKlineAdapterIntegration, FetchHistoricalSevenDaysAgo) {
    if (!integrationEnabled()) {
        GTEST_SKIP() << "Set BINANCE_INTEGRATION_TEST=1 to run live network test.";
    }

    BinanceContext ctx(publicReadOnlyConfig());
    RestClient rest = ctx.makeRestClient();
    backtest::RestClientKlineAdapter adapter(rest, ctx.ioc());

    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t sevenDaysAgoMs = nowMs - 7LL * 24LL * 60LL * 60LL * 1000LL;
    const int64_t targetOpen = (sevenDaysAgoMs / k30mMs) * k30mMs;

    auto result = adapter.fetchClosedKlines(
        "BTCUSDT", "30m", targetOpen, 20,
        std::chrono::milliseconds(10000), 3);

    ASSERT_TRUE(result.success) << result.errorMessage;
    ASSERT_FALSE(result.bars.empty());
    EXPECT_EQ(result.bars.back().openTime, targetOpen);
}
