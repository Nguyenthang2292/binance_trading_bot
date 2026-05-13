#include <gtest/gtest.h>

#include "orders/order_journal.h"

#include <filesystem>

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
