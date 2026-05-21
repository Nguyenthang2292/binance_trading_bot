#include <gtest/gtest.h>

#include "engine/work_queue.h"
#include "logger.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

std::vector<PairBlock> expectedBlocks(
    const std::vector<std::string>& symbols,
    const strategy::StrategyRegistry& registry,
    uint64_t seed) {
    std::vector<PairBlock> out;
    auto strategies = registry.all();
    if (strategies.empty() || symbols.empty()) {
        return out;
    }

    std::mt19937_64 rng(seed);
    std::shuffle(strategies.begin(), strategies.end(), rng);
    auto shuffledSymbols = symbols;
    std::shuffle(shuffledSymbols.begin(), shuffledSymbols.end(), rng);

    out.reserve(strategies.size() * shuffledSymbols.size());
    for (const auto* strategy : strategies) {
        if (!strategy) {
            continue;
        }
        const auto& intervals = strategy->config().intervals;
        if (intervals.empty()) {
            continue;
        }
        for (const auto& symbol : shuffledSymbols) {
            out.push_back(PairBlock{
                .symbol = symbol,
                .strategyName = strategy->config().name,
                .intervals = intervals,
            });
        }
    }
    return out;
}

void expectQueueMatchesExpected(
    const std::vector<engine::WorkItem>& queue,
    const std::vector<std::string>& symbols,
    const strategy::StrategyRegistry& registry,
    uint64_t seed) {
    const auto actual = toBlocks(queue);
    const auto expected = expectedBlocks(symbols, registry, seed);
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i].symbol, expected[i].symbol);
        EXPECT_EQ(actual[i].strategyName, expected[i].strategyName);
        EXPECT_EQ(actual[i].intervals, expected[i].intervals);
    }
}

} // namespace

TEST(WorkQueueTest, SeededShuffleRandomizesStrategyOrder) {
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
    c.intervals = {"15m"};
    registry.add(std::make_unique<QueueStrategy>(c));

    const uint64_t seed = 42u;
    const std::vector<std::string> symbols{"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry, seed);
    expectQueueMatchesExpected(queue, symbols, registry, seed);
}

TEST(WorkQueueTest, SameSeedProducesSameStrategyAndSymbolOrder) {
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

TEST(WorkQueueTest, SeededDualShuffleUsesStrategyThenSymbolOrder) {
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

    const uint64_t seed = 9u;
    const std::vector<std::string> symbols{"S1", "S2", "S3", "S4"};
    const auto queue = engine::WorkQueue::build(symbols, registry, seed);
    expectQueueMatchesExpected(queue, symbols, registry, seed);
}

TEST(WorkQueueTest, AllStrategiesHavePositiveSelectionProbability) {
    strategy::StrategyRegistry registry;

    for (const std::string& name : {"s0", "s1", "s2", "s3", "s4"}) {
        strategy::StrategyConfig cfg;
        cfg.name = name;
        cfg.intervals = {"1h"};
        registry.add(std::make_unique<QueueStrategy>(cfg));
    }

    const std::vector<std::string> symbols{"BTCUSDT", "ETHUSDT"};
    std::set<std::string> observed;
    for (const uint64_t seed : {1u, 2u, 3u, 4u, 5u, 11u, 17u, 23u, 31u, 47u}) {
        const auto queue = engine::WorkQueue::build(symbols, registry, seed);
        for (const auto& block : toBlocks(queue)) {
            observed.insert(block.strategyName);
        }
    }

    EXPECT_EQ(observed.size(), 5u);
}

TEST(WorkQueueTest, EveryStrategyCoversEverySymbolAfterShuffle) {
    strategy::StrategyRegistry registry;

    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"15m", "1h"};
    registry.add(std::make_unique<QueueStrategy>(a));

    strategy::StrategyConfig b;
    b.name = "b";
    b.intervals = {"30m"};
    registry.add(std::make_unique<QueueStrategy>(b));

    const uint64_t seed = 99u;
    const std::vector<std::string> symbols{"S1", "S2", "S3", "S4"};
    const auto queue = engine::WorkQueue::build(symbols, registry, seed);
    expectQueueMatchesExpected(queue, symbols, registry, seed);

    std::unordered_map<std::string, size_t> assignments;
    for (const auto& block : toBlocks(queue)) {
        ++assignments[block.strategyName];
    }
    EXPECT_EQ(assignments["a"], symbols.size());
    EXPECT_EQ(assignments["b"], symbols.size());
}

TEST(WorkQueueTest, AllSymbolsAssignedForEachStrategyAfterDualShuffle) {
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

    const std::vector<std::string> symbols{
        "BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT", "XRPUSDT", "DOGEUSDT", "ADAUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry, 20260517u);
    const auto blocks = toBlocks(queue);

    ASSERT_EQ(blocks.size(), symbols.size() * 3u);
    std::set<std::string> assignedSymbols;
    for (const auto& block : blocks) {
        assignedSymbols.insert(block.symbol);
    }
    EXPECT_EQ(assignedSymbols.size(), symbols.size());
    for (const auto& symbol : symbols) {
        EXPECT_TRUE(assignedSymbols.contains(symbol));
    }
}

TEST(WorkQueueTest, DegenerateSingleStrategy) {
    strategy::StrategyRegistry registry;

    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"15m", "1h"};
    registry.add(std::make_unique<QueueStrategy>(a));

    const uint64_t seed = 9u;
    const std::vector<std::string> symbols{"ETHUSDT", "BTCUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry, seed);
    expectQueueMatchesExpected(queue, symbols, registry, seed);
}

TEST(WorkQueueTest, IncludesAllStrategiesWhenStrategiesExceedSymbols) {
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

    const uint64_t seed = 1u;
    const std::vector<std::string> symbols{"BTCUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry, seed);
    ASSERT_EQ(queue.size(), 3u);
    expectQueueMatchesExpected(queue, symbols, registry, seed);
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

    const uint64_t seed = 11u;
    const std::vector<std::string> symbols{"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry, seed);
    expectQueueMatchesExpected(queue, symbols, registry, seed);
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

    const uint64_t seed = 2u;
    const std::vector<std::string> symbols{"CCCUSDT", "AAAUSDT", "BBBUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry, seed);
    const auto blocks = toBlocks(queue);
    expectQueueMatchesExpected(queue, symbols, registry, seed);

    std::set<std::string> usedSymbols;
    std::set<std::string> usedStrategies;
    for (const auto& block : blocks) {
        usedSymbols.insert(block.symbol);
        usedStrategies.insert(block.strategyName);
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
