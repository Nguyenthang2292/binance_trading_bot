#include <gtest/gtest.h>
#include "orders/order_journal.h"
#include "orders/order_result.h"
#include <filesystem>
#include <variant>

TEST(Mql4PhaseATest, OrderMetadataPersistence) {
    const auto path = std::filesystem::temp_directory_path() / "mql4_journal_metadata_test.log";
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

        OrderMetadata metadata;
        metadata.magic = 42;
        metadata.timeframe = "1h";
        metadata.comment = "hello tab\tnewline\n";
        metadata.strategyTag = "strategy-A";
        entry.metadata = metadata;

        auto record = journal.recordIntent(entry);
        ASSERT_TRUE(record.has_value());
    }

    {
        DurableOrderJournal journal(path.string());
        auto found = journal.findByClientOrderId("cid-1");
        ASSERT_TRUE(found.has_value());
        ASSERT_TRUE(found->has_value());
        auto& entry = found->value();
        ASSERT_TRUE(entry.metadata.has_value());
        EXPECT_EQ(entry.metadata->magic, 42);
        EXPECT_EQ(entry.metadata->timeframe, "1h");
        EXPECT_EQ(entry.metadata->comment, "hello tab newline ");
        EXPECT_EQ(entry.metadata->strategyTag, "strategy-A");
    }

    std::filesystem::remove(path);
}

TEST(Mql4PhaseATest, OrderPoolSnapshotHelpers) {
    OrderPoolSnapshot snapshot;
    snapshot.kind = OrderPoolKind::Open;
    snapshot.capturedAt = std::chrono::system_clock::now();

    OrderView v1;
    v1.identity = NormalOrderIdentity{.symbol = "BTCUSDT", .orderId = 123, .clientOrderId = "cid-1"};
    v1.normal.symbol = "BTCUSDT";
    v1.normal.orderId = 123;
    v1.normal.clientOrderId = "cid-1";

    OrderView v2;
    v2.identity = NormalOrderIdentity{.symbol = "ETHUSDT", .orderId = 456, .clientOrderId = "cid-2"};
    v2.normal.symbol = "ETHUSDT";
    v2.normal.orderId = 456;
    v2.normal.clientOrderId = "cid-2";

    snapshot.orders.push_back(v1);
    snapshot.orders.push_back(v2);

    EXPECT_EQ(snapshot.count(), 2);
    EXPECT_EQ(snapshot.items().size(), 2);

    auto at0 = snapshot.atSnapshotIndex(0);
    ASSERT_TRUE(at0.has_value());
    EXPECT_EQ(std::get<NormalOrderIdentity>(at0->identity).orderId, 123);

    auto at1 = snapshot.atSnapshotIndex(1);
    ASSERT_TRUE(at1.has_value());
    EXPECT_EQ(std::get<NormalOrderIdentity>(at1->identity).orderId, 456);

    auto at2 = snapshot.atSnapshotIndex(2);
    EXPECT_FALSE(at2.has_value());

    auto byId = snapshot.byIdentity(NormalOrderIdentity{.symbol = "ETHUSDT", .orderId = 456, .clientOrderId = "cid-2"});
    ASSERT_TRUE(byId.has_value());
    EXPECT_EQ(std::get<NormalOrderIdentity>(byId->identity).clientOrderId, "cid-2");

    auto byIdNotFound = snapshot.byIdentity(NormalOrderIdentity{.symbol = "BTCUSDT", .orderId = 999, .clientOrderId = "none"});
    EXPECT_FALSE(byIdNotFound.has_value());
}

TEST(Mql4PhaseATest, FormatOrderViewRedaction) {
    OrderView view;
    view.normal.symbol = "BTCUSDT";
    view.normal.orderId = 123;
    view.normal.status = "NEW";
    view.normal.side = OrderSide::Buy;
    view.normal.type = OrderType::Limit;
    view.normal.origQty = "1.0";
    view.normal.executedQty = "0.0";
    view.normal.price = "50000";
    
    view.metadata = OrderMetadata{.magic = 123, .timeframe = "4h", .comment = "secret", .strategyTag = "tag-X"};

    std::string formatted = formatOrderView(view, false);
    EXPECT_TRUE(formatted.find("magic=123") != std::string::npos);
    EXPECT_TRUE(formatted.find("metadata=[REDACTED]") != std::string::npos);
    EXPECT_TRUE(formatted.find("secret") == std::string::npos);
    EXPECT_TRUE(formatted.find("tag-X") == std::string::npos);

    std::string formattedFull = formatOrderView(view, true);
    EXPECT_TRUE(formattedFull.find("comment=\"secret\"") != std::string::npos);
    EXPECT_TRUE(formattedFull.find("tag=\"tag-X\"") != std::string::npos);
    EXPECT_TRUE(formattedFull.find("tf=\"4h\"") != std::string::npos);
}
