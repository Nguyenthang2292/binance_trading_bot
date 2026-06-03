#include <gtest/gtest.h>

#include "scanner/market_scanner.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

namespace {

template <typename T>
T runAwaitable(boost::asio::io_context& ioc, boost::asio::awaitable<T> task) {
    auto fut = boost::asio::co_spawn(ioc, std::move(task), boost::asio::use_future);
    ioc.run();
    return fut.get();
}

}  // namespace

TEST(MarketScannerTest, FiltersTradableUsdtPerpetualSymbols) {
    ExchangeSymbol tradable;
    tradable.symbol = "BTCUSDT";
    tradable.quoteAsset = "USDT";
    tradable.contractType = "PERPETUAL";
    tradable.status = "TRADING";

    ExchangeSymbol coinMargined = tradable;
    coinMargined.symbol = "BTCUSD_PERP";
    coinMargined.quoteAsset = "USD";

    ExchangeSymbol quarterly = tradable;
    quarterly.symbol = "ETHUSDT_240628";
    quarterly.contractType = "CURRENT_QUARTER";

    ExchangeSymbol halted = tradable;
    halted.symbol = "OLDUSDT";
    halted.status = "BREAK";

    ExchangeSymbol usdcUsdt = tradable;
    usdcUsdt.symbol = "USDCUSDT";

    const auto symbols = scanner::MarketScanner::tradableUsdtPerpetualSymbols({
        tradable,
        coinMargined,
        quarterly,
        halted,
        usdcUsdt,
    });

    ASSERT_EQ(symbols.size(), 1u);
    EXPECT_EQ(symbols[0], "BTCUSDT");
}

TEST(MarketScannerTest, ComputesStreamConnectionCount) {
    EXPECT_EQ(scanner::MarketScanner::streamConnectionCount(0, 2, 512), 0u);
    EXPECT_EQ(scanner::MarketScanner::streamConnectionCount(2000, 2, 512), 8u);
    EXPECT_EQ(scanner::MarketScanner::streamConnectionCount(3, 2, 4), 2u);
    EXPECT_EQ(scanner::MarketScanner::streamConnectionCount(3, 2, 0), 6u);
}

TEST(MarketScannerTest, WaitForConnectionsReadySucceedsWhenAllReady) {
    boost::asio::io_context ioc;
    auto ready = std::make_shared<std::atomic_bool>(true);
    const std::vector<std::shared_ptr<std::atomic_bool>> flags{ready};

    const auto result = runAwaitable(
        ioc,
        scanner::MarketScanner::waitForConnectionsReady(ioc, flags, std::chrono::milliseconds{50}));
    EXPECT_TRUE(result.has_value());
}

TEST(MarketScannerTest, WaitForConnectionsReadyFailsOnTimeout) {
    boost::asio::io_context ioc;
    auto ready = std::make_shared<std::atomic_bool>(false);
    const std::vector<std::shared_ptr<std::atomic_bool>> flags{ready};

    const auto result = runAwaitable(
        ioc,
        scanner::MarketScanner::waitForConnectionsReady(ioc, flags, std::chrono::milliseconds{10}));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, -91004);
}

