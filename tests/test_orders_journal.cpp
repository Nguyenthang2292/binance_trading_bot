#include <gtest/gtest.h>

#include "orders/order_journal.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

TEST(OrderJournalTest, DurableJournalPersistsEntryAcrossInstances) {
    const auto path = std::filesystem::temp_directory_path() / "orders_journal_test.log";
    std::filesystem::remove(path);

    {
        DurableOrderJournal journal(path.string());
        JournalEntry entry;
        entry.correlationId = "corr-1";
        entry.symbol = "BTCUSDT";
        entry.clientOrderId = "cid-1";
        entry.orderCategory = "normal";
        entry.quantity = "0.01";
        entry.price = "0";
        entry.requestParams = "symbol=BTCUSDT";
        entry.sendTimestampMs = 1700000000000;
        entry.state = PlacementState::UnknownPendingReconcile;

        auto record = journal.recordIntent(entry);
        ASSERT_TRUE(record.has_value());
        auto update = journal.updateState("corr-1", PlacementState::Accepted, 12345);
        ASSERT_TRUE(update.has_value());
    }

    {
        DurableOrderJournal journal(path.string());
        auto found = journal.findByClientOrderId("cid-1");
        ASSERT_TRUE(found.has_value());
        ASSERT_TRUE(found->has_value());
        EXPECT_EQ(found->value().correlationId, "corr-1");
        EXPECT_EQ(found->value().state, PlacementState::Accepted);
        ASSERT_TRUE(found->value().binanceOrderId.has_value());
        EXPECT_EQ(*found->value().binanceOrderId, 12345);
    }

    std::filesystem::remove(path);
}

TEST(OrderJournalTest, LoadsLegacyMetadataAndDerivesTimeframeFromComment) {
    const auto path = std::filesystem::temp_directory_path() / "orders_journal_legacy_metadata_test.log";
    std::filesystem::remove(path);

    {
        std::ofstream out(path.string(), std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << "R\tcorr-legacy\tBTCUSDT\tcid-legacy\tnormal\t0\t1\t0\t0.01\t0\tsymbol=BTCUSDT\t1700000000000\t2\t\t42\t"
            << "tf=15m reason=legacy\tlegacy-tag\n";
    }

    DurableOrderJournal journal(path.string());
    auto found = journal.findByClientOrderId("cid-legacy");
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found->has_value());
    ASSERT_TRUE(found->value().metadata.has_value());
    EXPECT_EQ(found->value().metadata->magic, 42);
    EXPECT_EQ(found->value().metadata->comment, "tf=15m reason=legacy");
    EXPECT_EQ(found->value().metadata->strategyTag, "legacy-tag");
    EXPECT_EQ(found->value().metadata->timeframe, "15m");

    std::filesystem::remove(path);
}

TEST(OrderJournalTest, ConstructorThrowsWhenPathIsNotWritableFile) {
    const auto path = std::filesystem::temp_directory_path() / "orders_journal_ctor_directory";
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);

    EXPECT_THROW(
        {
            DurableOrderJournal journal(path.string());
        },
        std::runtime_error);

    std::filesystem::remove_all(path);
}

TEST(OrderJournalTest, IgnoresCorruptLinesButLoadsValidEntries) {
    const auto path = std::filesystem::temp_directory_path() / "orders_journal_corrupt_lines_test.log";
    std::filesystem::remove(path);

    {
        std::ofstream out(path.string(), std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << "R\tcorr-good\tBTCUSDT\tcid-good\tnormal\t0\t1\t0\t0.01\t0\tsymbol=BTCUSDT\t1700000000000\t2\t\n";
        out << "R\tcorr-bad\tBTCUSDT\tcid-bad\tnormal\tINVALID_INT\t1\t0\t0.01\t0\tsymbol=BTCUSDT\t1700000000000\t2\t\n";
    }

    DurableOrderJournal journal(path.string());
    auto foundGood = journal.findByClientOrderId("cid-good");
    ASSERT_TRUE(foundGood.has_value());
    ASSERT_TRUE(foundGood->has_value());
    EXPECT_EQ(foundGood->value().correlationId, "corr-good");

    auto foundBad = journal.findByClientOrderId("cid-bad");
    ASSERT_TRUE(foundBad.has_value());
    EXPECT_FALSE(foundBad->has_value());

    std::filesystem::remove(path);
}
