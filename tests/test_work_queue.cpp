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

TEST(WorkQueueTest, BuildsStrategyMajorOrder) {
    strategy::StrategyRegistry registry;

    strategy::StrategyConfig a;
    a.name = "a";
    a.intervals = {"15m"};
    registry.add(std::make_unique<QueueStrategy>(a));

    strategy::StrategyConfig b;
    b.name = "b";
    b.intervals = {"30m"};
    registry.add(std::make_unique<QueueStrategy>(b));

    const std::vector<std::string> symbols{"BTCUSDT", "ETHUSDT"};
    const auto queue = engine::WorkQueue::build(symbols, registry);
    ASSERT_EQ(queue.size(), 4u);

    EXPECT_EQ(queue[0].strategy->config().name, "a");
    EXPECT_EQ(queue[1].strategy->config().name, "a");
    EXPECT_EQ(queue[2].strategy->config().name, "b");
    EXPECT_EQ(queue[3].strategy->config().name, "b");
    EXPECT_EQ(queue[0].interval, "15m");
    EXPECT_EQ(queue[2].interval, "30m");
}

