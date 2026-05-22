#include <gtest/gtest.h>

#include "catalog/plugin_handle.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* kPluginFilename = "strategy_qlib_strategy_signal.dll";

std::filesystem::path findPluginPathFrom(std::filesystem::path start) {
    if (start.empty()) {
        return {};
    }
    std::error_code ec;
    start = std::filesystem::absolute(start, ec);
    if (ec) {
        return {};
    }

    auto current = std::move(start);
    while (!current.empty()) {
        const auto candidate = current / "plugins" / kPluginFilename;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }

        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

std::filesystem::path pluginPath() {
    if (const auto byCwd = findPluginPathFrom(std::filesystem::current_path()); !byCwd.empty()) {
        return byCwd;
    }
    if (const auto byFile = findPluginPathFrom(std::filesystem::path(__FILE__).parent_path()); !byFile.empty()) {
        return byFile;
    }
    throw std::runtime_error("unable to locate strategy_qlib_strategy_signal.dll");
}

catalog::PluginHandle loadPlugin() {
    auto loaded = catalog::PluginHandle::load(pluginPath());
    if (!loaded) {
        throw std::runtime_error(loaded.error());
    }
    return std::move(*loaded);
}

using StrategyPtr = std::unique_ptr<strategy::IStrategy, catalog::PluginHandle::DestroyFn>;

} // namespace

TEST(QlibStrategySignalPluginTest, CatalogCreationDoesNotRequireInitializedDb) {
    auto plugin = loadPlugin();
    const std::string config = R"json(
{
  "name": "Qlib TopK Dropout 30m Adapter",
  "type": "qlib_strategy_signal",
  "intervals": ["30m"],
  "params": {
    "source": "sqlite",
    "db_path": "data/qlib_smoke/uninitialized_for_catalog_test.db",
    "strategy_id": "topk_dropout_30m_v1",
    "universe_hash_strict": false
  }
}
)json";

    strategy::IStrategy* raw = plugin.create(config.c_str());
    ASSERT_NE(raw, nullptr);
    StrategyPtr strategy(raw, plugin.destroyFunction());

    EXPECT_EQ(strategy->config().type, "qlib_strategy_signal");
    EXPECT_EQ(strategy->config().adapterId, "topk_dropout_30m_v1");

    const auto signal = strategy->evaluate("BTCUSDT", "30m", std::vector<Kline>{});
    EXPECT_EQ(signal.direction, strategy::Signal::Direction::None);
    EXPECT_NE(signal.reason.find("schema_unavailable"), std::string::npos);
}
