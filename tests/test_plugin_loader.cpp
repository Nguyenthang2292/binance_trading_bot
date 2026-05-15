#include <gtest/gtest.h>

#include "catalog/plugin_loader.h"

namespace {

class DummyStrategy final : public strategy::IStrategy {
public:
    explicit DummyStrategy(strategy::StrategyConfig cfg) : m_cfg(std::move(cfg)) {}
    const strategy::StrategyConfig& config() const override { return m_cfg; }
    strategy::Signal evaluate(std::string_view, std::string_view, const std::vector<Kline>&) const override {
        return {};
    }

private:
    strategy::StrategyConfig m_cfg;
};

strategy::IStrategy* createOk(const char*) {
    strategy::StrategyConfig cfg;
    cfg.name = "dummy";
    return new DummyStrategy(cfg);
}
strategy::IStrategy* createNull(const char*) { return nullptr; }
void destroyOk(strategy::IStrategy* ptr) { delete ptr; }
const char* typeFn() { return "dummy"; }
const char* verFn() { return "1.0.0"; }

} // namespace

TEST(PluginLoaderTest, LoadAllAndCreateSuccess) {
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins"},
        [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{"a.dll"}; },
        [](const std::filesystem::path& path) {
            return std::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path, &createOk, &destroyOk, &typeFn, &verFn, "dummy", "1.0.0"));
        });

    const auto results = loader.loadAll();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].success);
    EXPECT_EQ(results[0].type, "dummy");

    auto instance = loader.createStrategy("dummy", "{}");
    ASSERT_TRUE(instance);
    EXPECT_EQ(instance->config().name, "dummy");
}

TEST(PluginLoaderTest, CapturesMissingExportFailure) {
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins"},
        [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{"bad.dll"}; },
        [](const std::filesystem::path&) {
            return std::expected<catalog::PluginHandle, std::string>(
                std::unexpected(std::string("missing required exports")));
        });

    const auto results = loader.loadAll();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].success);
    EXPECT_NE(results[0].error.find("missing"), std::string::npos);
}

TEST(PluginLoaderTest, CreateReturnsNullWhenFactoryReturnsNullptr) {
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins"},
        [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{"null.dll"}; },
        [](const std::filesystem::path& path) {
            return std::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path, &createNull, &destroyOk, &typeFn, &verFn, "dummy", "1.0.0"));
        });

    (void)loader.loadAll();
    auto instance = loader.createStrategy("dummy", "{}");
    EXPECT_FALSE(instance);
}

