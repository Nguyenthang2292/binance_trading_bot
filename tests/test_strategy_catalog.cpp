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
            return compat::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
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
            return compat::expected<catalog::PluginHandle, std::string>(
                compat::unexpected(std::string("not used")));
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

TEST(StrategyCatalogTest, InitializeTwiceClearsRegistryBeforeReload) {
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins"},
        [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{"rsi.dll"}; },
        [](const std::filesystem::path& path) {
            return compat::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path,
                &createCatalogStrategy,
                &destroyCatalogStrategy,
                &catalogTypeFn,
                &catalogVersionFn,
                "rsi_reversal",
                "1.2.3"));
        });

    strategy::StrategyRegistry registry;
    catalog::StrategyCatalog catalog({.pluginsDir = "plugins"}, registry, std::move(loader));

    const std::vector<nlohmann::json> firstCfg{
        nlohmann::json{
            {"name", "first"},
            {"type", "rsi_reversal"},
            {"intervals", nlohmann::json::array({"15m"})},
        }};
    const auto first = catalog.initialize(firstCfg);
    EXPECT_EQ(first.strategiesRegistered, 1);
    ASSERT_EQ(registry.all().size(), 1u);

    const std::vector<nlohmann::json> secondCfg{
        nlohmann::json{
            {"name", "second"},
            {"type", "rsi_reversal"},
            {"intervals", nlohmann::json::array({"1h"})},
        }};
    const auto second = catalog.initialize(secondCfg);
    EXPECT_EQ(second.strategiesRegistered, 1);
    EXPECT_TRUE(second.errors.empty());
    ASSERT_EQ(registry.all().size(), 1u);

    const auto info = catalog.listStrategies();
    ASSERT_EQ(info.size(), 1u);
    EXPECT_EQ(info[0].name, "second");
    EXPECT_EQ(info[0].type, "rsi_reversal");
}

TEST(StrategyCatalogTest, NonStringTypeIsReportedAsValidationError) {
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins"},
        [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{}; },
        [](const std::filesystem::path&) {
            return compat::expected<catalog::PluginHandle, std::string>(
                compat::unexpected(std::string("not used")));
        });

    strategy::StrategyRegistry registry;
    catalog::StrategyCatalog catalog({.pluginsDir = "plugins"}, registry, std::move(loader));
    const std::vector<nlohmann::json> cfg{
        nlohmann::json{
            {"name", "bad_type"},
            {"type", 123},
        }};

    const auto summary = catalog.initialize(cfg);
    EXPECT_EQ(summary.strategiesRegistered, 0);
    ASSERT_EQ(summary.errors.size(), 1u);
    EXPECT_EQ(summary.errors[0], "strategy config 'type' must be a string");
}

TEST(StrategyCatalogTest, InvalidStrategyConfigIsRejectedBeforeRegistryInsert) {
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins"},
        [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{"rsi.dll"}; },
        [](const std::filesystem::path& path) {
            return compat::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path,
                &createCatalogStrategy,
                &destroyCatalogStrategy,
                &catalogTypeFn,
                &catalogVersionFn,
                "rsi_reversal",
                "1.2.3"));
        });

    strategy::StrategyRegistry registry;
    catalog::StrategyCatalog catalog({.pluginsDir = "plugins"}, registry, std::move(loader));
    const std::vector<nlohmann::json> cfg{
        nlohmann::json{
            {"name", "bad_intervals"},
            {"type", "rsi_reversal"},
            {"intervals", nlohmann::json::array()},
        }};

    const auto summary = catalog.initialize(cfg);
    EXPECT_EQ(summary.strategiesRegistered, 0);
    ASSERT_FALSE(summary.errors.empty());
    EXPECT_NE(summary.errors[0].find("invalid strategy config"), std::string::npos);
    EXPECT_TRUE(registry.all().empty());
}

TEST(StrategyCatalogTest, PartialRegistrationFailsClosed) {
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins"},
        [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{"rsi.dll"}; },
        [](const std::filesystem::path& path) {
            return compat::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path,
                &createCatalogStrategy,
                &destroyCatalogStrategy,
                &catalogTypeFn,
                &catalogVersionFn,
                "rsi_reversal",
                "1.2.3"));
        });

    strategy::StrategyRegistry registry;
    catalog::StrategyCatalog catalog({.pluginsDir = "plugins"}, registry, std::move(loader));
    const std::vector<nlohmann::json> cfg{
        nlohmann::json{
            {"name", "valid"},
            {"type", "rsi_reversal"},
            {"intervals", nlohmann::json::array({"15m"})},
        },
        nlohmann::json{
            {"name", "bad"},
            {"type", "rsi_reversal"},
            {"intervals", nlohmann::json::array()},
        }};

    const auto summary = catalog.initialize(cfg);
    EXPECT_EQ(summary.strategiesRegistered, 0);
    ASSERT_FALSE(summary.errors.empty());
    EXPECT_TRUE(registry.all().empty());
    EXPECT_TRUE(catalog.listStrategies().empty());
}

