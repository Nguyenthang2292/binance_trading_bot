#include <gtest/gtest.h>

#include "binance_api.h"

TEST(BinanceApiTest, LegacyOrdersConfigUsesDurableJournalAndResultResponse) {
    const auto cfg = BinanceAPI::makeLegacyOrdersConfig();
    EXPECT_TRUE(cfg.journalIsDurable);
    EXPECT_EQ(cfg.defaultResponseType, ResponseType::RESULT);
    EXPECT_FALSE(cfg.journalPath.empty());
}

TEST(BinanceApiTest, LegacyOrdersConfigDisablesBestEffortAndTimestampOverride) {
    const auto cfg = BinanceAPI::makeLegacyOrdersConfig();
    EXPECT_FALSE(cfg.allowBestEffortJournal);
    EXPECT_FALSE(cfg.allowRawTimestampOverride);
    EXPECT_EQ(cfg.positionMode, PositionMode::OneWay);
}
