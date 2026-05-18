#include <gtest/gtest.h>

#include "engine/work_queue.h"

#include <array>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

namespace {

class QueueStrategy final : public strategy::IStrategy {
public:
    explicit QueueStrategy(strategy::StrategyConfig cfg) : m_cfg(std::move(cfg)) {}
    const strategy::StrategyConfig& config() const override { return m_cfg; }
    strategy::Signal evaluate(std::string_view, std::string_view, const std::vector<Kline>&) const override {
        return {};
    }

private:
    strategy::StrategyConfig m_cfg;
};

struct PairBlock {
    std::string symbol;
    std::string strategyName;
    std::vector<std::string> intervals;
};

std::vector<PairBlock> toBlocks(const std::vector<engine::WorkItem>& queue) {
    std::vector<PairBlock> out;
    for (const auto& item : queue) {
        const std::string strategyName = item.strategy ? item.strategy->config().name : "";
        if (out.empty() || out.back().symbol != item.symbol || out.back().strategyName != strategyName) {
            out.push_back(PairBlock{
                .symbol = item.symbol,
                .strategyName = strategyName,
                .intervals = {},
            });
        }
        out.back().intervals.push_back(item.interval);
    }
    return out;
}

std::vector<std::string> symbolsInOrder(const std::vector<engine::WorkItem>& queue) {
    std::vector<std::string> out;
    out.reserve(queue.size());
    for (const auto& item : queue) {
        out.push_back(item.symbol);
    }
    return out;
}

} // namespace

TEST(WorkQueueTest, SeededShufflePreservesZipAssignments) {
    strategy::StrategyRegistry registry;

    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"1h", "4h"};
    registry.add(std::make_unique<QueueStrategy>(a));

    strategy::StrategyConfig b;
    b.name = "b";
    b.intervals = {"30m"};
    registry.add(std::make_unique<QueueStrategy>(b));

    const std::vector<std::string> symbols{"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry, 42u);
    ASSERT_EQ(queue.size(), 5u);

    const auto blocks = toBlocks(queue);
    ASSERT_EQ(blocks.size(), 3u);
    EXPECT_EQ(blocks[0].strategyName, "a");
    EXPECT_EQ(blocks[0].intervals, a.intervals);
    EXPECT_EQ(blocks[1].strategyName, "b");
    EXPECT_EQ(blocks[1].intervals, b.intervals);
    EXPECT_EQ(blocks[2].strategyName, "a");
    EXPECT_EQ(blocks[2].intervals, a.intervals);

    std::set<std::string> usedSymbols;
    for (const auto& block : blocks) {
        usedSymbols.insert(block.symbol);
    }
    EXPECT_EQ(usedSymbols.size(), symbols.size());
    for (const auto& symbol : symbols) {
        EXPECT_TRUE(usedSymbols.contains(symbol));
    }
}

TEST(WorkQueueTest, SameSeedProducesSameOrder) {
    strategy::StrategyRegistry registry;

    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"1h", "4h"};
    registry.add(std::make_unique<QueueStrategy>(a));

    strategy::StrategyConfig b;
    b.name = "b";
    b.intervals = {"30m"};
    registry.add(std::make_unique<QueueStrategy>(b));

    const std::vector<std::string> symbols{"BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT"};
    const auto queueA = engine::WorkQueue::build(symbols, registry, 777u);
    const auto queueB = engine::WorkQueue::build(symbols, registry, 777u);

    ASSERT_EQ(queueA.size(), queueB.size());
    for (size_t i = 0; i < queueA.size(); ++i) {
        EXPECT_EQ(queueA[i].symbol, queueB[i].symbol);
        EXPECT_EQ(queueA[i].interval, queueB[i].interval);
        ASSERT_NE(queueA[i].strategy, nullptr);
        ASSERT_NE(queueB[i].strategy, nullptr);
        EXPECT_EQ(queueA[i].strategy->config().name, queueB[i].strategy->config().name);
    }
}

TEST(WorkQueueTest, FixedSeedCanProduceNonInputOrder) {
    strategy::StrategyRegistry registry;

    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"1h"};
    registry.add(std::make_unique<QueueStrategy>(a));

    const std::vector<std::string> symbols{
        "S01USDT", "S02USDT", "S03USDT", "S04USDT", "S05USDT",
        "S06USDT", "S07USDT", "S08USDT", "S09USDT", "S10USDT"};

    bool sawReordered = false;
    for (const uint64_t seed : std::array<uint64_t, 7>{1u, 2u, 3u, 4u, 5u, 42u, 99u}) {
        const auto queue = engine::WorkQueue::build(symbols, registry, seed);
        if (symbolsInOrder(queue) != symbols) {
            sawReordered = true;
            break;
        }
    }

    EXPECT_TRUE(sawReordered);
}

TEST(WorkQueueTest, AllSymbolsAreAssignedExactlyOnce) {
    strategy::StrategyRegistry registry;

    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"1h", "4h"};
    registry.add(std::make_unique<QueueStrategy>(a));

    strategy::StrategyConfig b;
    b.name = "b";
    b.intervals = {"30m"};
    registry.add(std::make_unique<QueueStrategy>(b));

    strategy::StrategyConfig c;
    c.name = "c";
    c.intervals = {"15m", "1d", "1w"};
    registry.add(std::make_unique<QueueStrategy>(c));

    const std::unordered_map<std::string, size_t> intervalCountByStrategy{
        {"a", a.intervals.size()},
        {"b", b.intervals.size()},
        {"c", c.intervals.size()},
    };

    const std::vector<std::string> symbols{
        "BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT", "XRPUSDT", "DOGEUSDT", "ADAUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry, 20260517u);
    const auto blocks = toBlocks(queue);

    ASSERT_EQ(blocks.size(), symbols.size());
    std::set<std::string> assignedSymbols;
    for (const auto& block : blocks) {
        assignedSymbols.insert(block.symbol);
        const auto it = intervalCountByStrategy.find(block.strategyName);
        ASSERT_NE(it, intervalCountByStrategy.end());
        EXPECT_EQ(block.intervals.size(), it->second);
    }

    EXPECT_EQ(assignedSymbols.size(), symbols.size());
    for (const auto& symbol : symbols) {
        EXPECT_TRUE(assignedSymbols.contains(symbol));
    }
}

TEST(WorkQueueTest, DegeneratesToSingleStrategyAcrossAllSymbols) {
    strategy::StrategyRegistry registry;

    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"15m", "1h"};
    registry.add(std::make_unique<QueueStrategy>(a));

    const std::vector<std::string> symbols{"ETHUSDT", "BTCUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry, 9u);
    ASSERT_EQ(queue.size(), 4u);

    const auto blocks = toBlocks(queue);
    ASSERT_EQ(blocks.size(), symbols.size());
    for (const auto& block : blocks) {
        EXPECT_EQ(block.strategyName, "a");
        EXPECT_EQ(block.intervals, a.intervals);
    }
}

TEST(WorkQueueTest, SkipsUnpairedStrategiesWhenStrategiesExceedSymbols) {
    strategy::StrategyRegistry registry;

    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"15m"};
    registry.add(std::make_unique<QueueStrategy>(a));

    strategy::StrategyConfig b;
    b.name = "b";
    b.intervals = {"30m"};
    registry.add(std::make_unique<QueueStrategy>(b));

    strategy::StrategyConfig c;
    c.name = "c";
    c.intervals = {"1h"};
    registry.add(std::make_unique<QueueStrategy>(c));

    const std::vector<std::string> symbols{"BTCUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry, 1u);
    ASSERT_EQ(queue.size(), 1u);

    EXPECT_EQ(queue[0].symbol, "BTCUSDT");
    ASSERT_NE(queue[0].strategy, nullptr);
    EXPECT_EQ(queue[0].strategy->config().name, "a");
    EXPECT_EQ(queue[0].interval, "15m");
}

TEST(WorkQueueTest, WarnsWhenStrategiesExceedSymbols) {
    strategy::StrategyRegistry registry;

    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"15m"};
    registry.add(std::make_unique<QueueStrategy>(a));

    strategy::StrategyConfig b;
    b.name = "b";
    b.intervals = {"30m"};
    registry.add(std::make_unique<QueueStrategy>(b));

    const std::vector<std::string> symbols{"BTCUSDT"};
    testing::internal::CaptureStdout();
    const auto queue = engine::WorkQueue::build(symbols, registry, 1u);
    const std::string output = testing::internal::GetCapturedStdout();

    ASSERT_EQ(queue.size(), 1u);
    EXPECT_NE(output.find("work queue strategy starvation this cycle"), std::string::npos);
}

TEST(WorkQueueTest, StrategyWithNoIntervalsProducesNoWorkItemsForItsPair) {
    strategy::StrategyRegistry registry;

    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"1h"};
    registry.add(std::make_unique<QueueStrategy>(a));

    strategy::StrategyConfig b;
    b.name = "b";
    b.intervals = {};
    registry.add(std::make_unique<QueueStrategy>(b));

    const std::vector<std::string> symbols{"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry, 11u);
    ASSERT_EQ(queue.size(), 2u);

    std::set<std::string> seenSymbols;
    for (const auto& item : queue) {
        seenSymbols.insert(item.symbol);
        ASSERT_NE(item.strategy, nullptr);
        EXPECT_EQ(item.strategy->config().name, "a");
        EXPECT_EQ(item.interval, "1h");
    }
    EXPECT_EQ(seenSymbols.size(), 2u);
}

TEST(WorkQueueTest, BuildsOneToOneMappingWhenCountsMatch) {
    strategy::StrategyRegistry registry;

    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"1m"};
    registry.add(std::make_unique<QueueStrategy>(a));

    strategy::StrategyConfig b;
    b.name = "b";
    b.intervals = {"3m"};
    registry.add(std::make_unique<QueueStrategy>(b));

    strategy::StrategyConfig c;
    c.name = "c";
    c.intervals = {"5m"};
    registry.add(std::make_unique<QueueStrategy>(c));

    const std::vector<std::string> symbols{"CCCUSDT", "AAAUSDT", "BBBUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry, 2u);
    ASSERT_EQ(queue.size(), 3u);

    std::set<std::string> usedSymbols;
    std::set<std::string> usedStrategies;
    for (const auto& item : queue) {
        usedSymbols.insert(item.symbol);
        ASSERT_NE(item.strategy, nullptr);
        usedStrategies.insert(item.strategy->config().name);
    }

    EXPECT_EQ(usedSymbols.size(), 3u);
    EXPECT_EQ(usedStrategies.size(), 3u);
}

TEST(WorkQueueTest, ReturnsEmptyWhenNoStrategies) {
    strategy::StrategyRegistry registry;
    const std::vector<std::string> symbols{"BTCUSDT", "ETHUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry, 42u);
    EXPECT_TRUE(queue.empty());
}

TEST(WorkQueueTest, ReturnsEmptyWhenNoSymbols) {
    strategy::StrategyRegistry registry;
    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"15m"};
    registry.add(std::make_unique<QueueStrategy>(a));

    const std::vector<std::string> symbols;
    const auto queue = engine::WorkQueue::build(symbols, registry, 42u);
    EXPECT_TRUE(queue.empty());
}

TEST(WorkQueueTest, NulloptSeedUsesRandomDevice) {
    strategy::StrategyRegistry registry;
    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"15m"};
    registry.add(std::make_unique<QueueStrategy>(a));

    const std::vector<std::string> symbols{"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    const auto queueA = engine::WorkQueue::build(symbols, registry, std::nullopt);
    const auto queueB = engine::WorkQueue::build(symbols, registry, std::nullopt);

    EXPECT_EQ(queueA.size(), symbols.size());
    EXPECT_EQ(queueB.size(), symbols.size());
}
