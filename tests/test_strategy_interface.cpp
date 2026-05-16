#include <gtest/gtest.h>

#include "strategy/strategy_registry.h"

namespace {

class MockStrategy final : public strategy::IStrategy {
public:
    explicit MockStrategy(strategy::StrategyConfig cfg) : m_cfg(std::move(cfg)) {}

    const strategy::StrategyConfig& config() const override { return m_cfg; }

    strategy::Signal evaluate(
        std::string_view,
        std::string_view,
        const std::vector<Kline>&) const override {
        return {};
    }

private:
    strategy::StrategyConfig m_cfg;
};

} // namespace

TEST(StrategyInterfaceTest, RegistryFiltersByInterval) {
    strategy::StrategyRegistry registry;

    strategy::StrategyConfig cfgA;
    cfgA.name = "a";
    cfgA.intervals = {"15m"};
    registry.add(std::make_unique<MockStrategy>(cfgA));

    strategy::StrategyConfig cfgB;
    cfgB.name = "b";
    cfgB.intervals = {"30m", "1h"};
    registry.add(std::make_unique<MockStrategy>(cfgB));

    const auto all = registry.all();
    ASSERT_EQ(all.size(), 2u);

    const auto by15 = registry.forInterval("15m");
    ASSERT_EQ(by15.size(), 1u);
    EXPECT_EQ(by15[0]->config().name, "a");

    const auto by30 = registry.forInterval("30m");
    ASSERT_EQ(by30.size(), 1u);
    EXPECT_EQ(by30[0]->config().name, "b");
}

TEST(StrategyInterfaceTest, RegistryIgnoresNullStrategies) {
    strategy::StrategyRegistry registry;

    registry.add(nullptr);
    registry.addShared(nullptr);

    EXPECT_TRUE(registry.all().empty());
    EXPECT_TRUE(registry.forInterval("1h").empty());
}

