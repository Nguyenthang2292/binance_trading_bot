#include <gtest/gtest.h>

#include "scanner/market_scanner.h"

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

    const auto symbols = scanner::MarketScanner::tradableUsdtPerpetualSymbols({
        tradable,
        coinMargined,
        quarterly,
        halted,
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

