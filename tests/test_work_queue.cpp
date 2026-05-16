#include <gtest/gtest.h>

#include "engine/work_queue.h"

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

} // namespace

TEST(WorkQueueTest, BuildsZipStrategySymbolOrder) {
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
    const auto queue = engine::WorkQueue::build(symbols, registry);
    ASSERT_EQ(queue.size(), 5u);

    EXPECT_EQ(queue[0].symbol, "BTCUSDT");
    EXPECT_EQ(queue[0].strategy->config().name, "a");
    EXPECT_EQ(queue[0].interval, "1h");

    EXPECT_EQ(queue[1].symbol, "BTCUSDT");
    EXPECT_EQ(queue[1].strategy->config().name, "a");
    EXPECT_EQ(queue[1].interval, "4h");

    EXPECT_EQ(queue[2].symbol, "ETHUSDT");
    EXPECT_EQ(queue[2].strategy->config().name, "b");
    EXPECT_EQ(queue[2].interval, "30m");

    EXPECT_EQ(queue[3].symbol, "SOLUSDT");
    EXPECT_EQ(queue[3].strategy->config().name, "a");
    EXPECT_EQ(queue[3].interval, "1h");

    EXPECT_EQ(queue[4].symbol, "SOLUSDT");
    EXPECT_EQ(queue[4].strategy->config().name, "a");
    EXPECT_EQ(queue[4].interval, "4h");
}

TEST(WorkQueueTest, SortsSymbolsForDeterministicZipOrder) {
    strategy::StrategyRegistry registry;

    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"1h"};
    registry.add(std::make_unique<QueueStrategy>(a));

    strategy::StrategyConfig b;
    b.name = "b";
    b.intervals = {"30m"};
    registry.add(std::make_unique<QueueStrategy>(b));

    const std::vector<std::string> symbols{"SOLUSDT", "BTCUSDT", "ETHUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry);
    ASSERT_EQ(queue.size(), 3u);

    EXPECT_EQ(queue[0].symbol, "BTCUSDT");
    EXPECT_EQ(queue[0].strategy->config().name, "a");

    EXPECT_EQ(queue[1].symbol, "ETHUSDT");
    EXPECT_EQ(queue[1].strategy->config().name, "b");

    EXPECT_EQ(queue[2].symbol, "SOLUSDT");
    EXPECT_EQ(queue[2].strategy->config().name, "a");
}

TEST(WorkQueueTest, DegeneratesToSingleStrategyAcrossAllSymbols) {
    strategy::StrategyRegistry registry;

    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"15m", "1h"};
    registry.add(std::make_unique<QueueStrategy>(a));

    const std::vector<std::string> symbols{"ETHUSDT", "BTCUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry);
    ASSERT_EQ(queue.size(), 4u);

    EXPECT_EQ(queue[0].symbol, "BTCUSDT");
    EXPECT_EQ(queue[0].strategy->config().name, "a");
    EXPECT_EQ(queue[0].interval, "15m");

    EXPECT_EQ(queue[1].symbol, "BTCUSDT");
    EXPECT_EQ(queue[1].strategy->config().name, "a");
    EXPECT_EQ(queue[1].interval, "1h");

    EXPECT_EQ(queue[2].symbol, "ETHUSDT");
    EXPECT_EQ(queue[2].strategy->config().name, "a");
    EXPECT_EQ(queue[2].interval, "15m");

    EXPECT_EQ(queue[3].symbol, "ETHUSDT");
    EXPECT_EQ(queue[3].strategy->config().name, "a");
    EXPECT_EQ(queue[3].interval, "1h");
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
    const auto queue = engine::WorkQueue::build(symbols, registry);
    ASSERT_EQ(queue.size(), 1u);

    EXPECT_EQ(queue[0].symbol, "BTCUSDT");
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
    const auto queue = engine::WorkQueue::build(symbols, registry);
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
    const auto queue = engine::WorkQueue::build(symbols, registry);
    ASSERT_EQ(queue.size(), 2u);

    EXPECT_EQ(queue[0].symbol, "BTCUSDT");
    EXPECT_EQ(queue[0].strategy->config().name, "a");
    EXPECT_EQ(queue[0].interval, "1h");

    EXPECT_EQ(queue[1].symbol, "SOLUSDT");
    EXPECT_EQ(queue[1].strategy->config().name, "a");
    EXPECT_EQ(queue[1].interval, "1h");
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
    const auto queue = engine::WorkQueue::build(symbols, registry);
    ASSERT_EQ(queue.size(), 3u);

    EXPECT_EQ(queue[0].symbol, "AAAUSDT");
    EXPECT_EQ(queue[0].strategy->config().name, "a");
    EXPECT_EQ(queue[0].interval, "1m");

    EXPECT_EQ(queue[1].symbol, "BBBUSDT");
    EXPECT_EQ(queue[1].strategy->config().name, "b");
    EXPECT_EQ(queue[1].interval, "3m");

    EXPECT_EQ(queue[2].symbol, "CCCUSDT");
    EXPECT_EQ(queue[2].strategy->config().name, "c");
    EXPECT_EQ(queue[2].interval, "5m");
}

TEST(WorkQueueTest, ReturnsEmptyWhenNoStrategies) {
    strategy::StrategyRegistry registry;
    const std::vector<std::string> symbols{"BTCUSDT", "ETHUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry);
    EXPECT_TRUE(queue.empty());
}

TEST(WorkQueueTest, ReturnsEmptyWhenNoSymbols) {
    strategy::StrategyRegistry registry;
    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"15m"};
    registry.add(std::make_unique<QueueStrategy>(a));

    const std::vector<std::string> symbols;
    const auto queue = engine::WorkQueue::build(symbols, registry);
    EXPECT_TRUE(queue.empty());
}

