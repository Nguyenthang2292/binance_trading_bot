#include <gtest/gtest.h>

#include "catalog/strategy_catalog.h"

#include <nlohmann/json.hpp>

namespace {

class CatalogStrategy final : public strategy::IStrategy {
public:
    explicit CatalogStrategy(strategy::StrategyConfig cfg) : m_cfg(std::move(cfg)) {}
    const strategy::StrategyConfig& config() const override { return m_cfg; }
    strategy::Signal evaluate(std::string_view, std::string_view, const std::vector<Kline>&) const override {
        return {};
    }

private:
    strategy::StrategyConfig m_cfg;
};

strategy::IStrategy* createCatalogStrategy(const char* configJson) {
    const auto j = nlohmann::json::parse(configJson);
    strategy::StrategyConfig cfg;
    cfg.name = j.value("name", "unknown");
    cfg.type = j.value("type", "");
    cfg.intervals = j.value("intervals", std::vector<std::string>{});
    return new CatalogStrategy(cfg);
}

void destroyCatalogStrategy(strategy::IStrategy* ptr) {
    delete ptr;
}

const char* catalogTypeFn() { return "rsi_reversal"; }
const char* catalogVersionFn() { return "1.2.3"; }

} // namespace

TEST(StrategyCatalogTest, InitializeRegistersStrategiesByType) {
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins"},
        [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{"rsi.dll"}; },
        [](const std::filesystem::path& path) {
            return std::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path,
                &createCatalogStrategy,
                &destroyCatalogStrategy,
                &catalogTypeFn,
                &catalogVersionFn,
                "rsi_reversal",
                "1.2.3"));
        });

    strategy::StrategyRegistry registry;
    catalog::StrategyCatalog catalog(
        {.pluginsDir = "plugins"},
        registry,
        std::move(loader));

    std::vector<nlohmann::json> cfg{
        nlohmann::json{
            {"name", "rsi_15m"},
            {"type", "rsi_reversal"},
            {"intervals", nlohmann::json::array({"15m"})},
        }};
    const auto summary = catalog.initialize(cfg);
    EXPECT_EQ(summary.pluginsFound, 1);
    EXPECT_EQ(summary.pluginsLoaded, 1);
    EXPECT_EQ(summary.strategiesRegistered, 1);
    EXPECT_TRUE(summary.errors.empty());
    EXPECT_EQ(registry.all().size(), 1u);

    const auto info = catalog.listStrategies();
    ASSERT_EQ(info.size(), 1u);
    EXPECT_EQ(info[0].name, "rsi_15m");
    EXPECT_EQ(info[0].type, "rsi_reversal");
}

TEST(StrategyCatalogTest, MissingPluginTypeReturnsError) {
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins"},
        [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{}; },
        [](const std::filesystem::path&) {
            return std::expected<catalog::PluginHandle, std::string>(
                std::unexpected(std::string("not used")));
        });

    strategy::StrategyRegistry registry;
    catalog::StrategyCatalog catalog({.pluginsDir = "plugins"}, registry, std::move(loader));
    std::vector<nlohmann::json> cfg{
        nlohmann::json{
            {"name", "rsi_15m"},
            {"type", "rsi_reversal"},
            {"intervals", nlohmann::json::array({"15m"})},
        }};

    const auto summary = catalog.initialize(cfg);
    EXPECT_EQ(summary.strategiesRegistered, 0);
    ASSERT_FALSE(summary.errors.empty());
}

