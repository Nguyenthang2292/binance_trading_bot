#pragma once

#include "catalog/plugin_loader.h"
#include "strategy/strategy_registry.h"

#include <filesystem>
#include <chrono>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace catalog {

class StrategyCatalog {
public:
    struct Config {
        std::filesystem::path pluginsDir{"plugins"};
    };

    struct LoadSummary {
        int pluginsFound{0};
        int pluginsLoaded{0};
        int strategiesRegistered{0};
        std::vector<std::string> errors;
    };

    struct StrategyInfo {
        std::string name;
        std::string type;
        std::string version;
        std::string pluginFile;
        std::vector<std::string> intervals;
        std::chrono::seconds scanInterval{0};
        std::chrono::seconds maxHoldDuration{0};
    };

    StrategyCatalog(Config config, strategy::StrategyRegistry& registry);
    StrategyCatalog(Config config, strategy::StrategyRegistry& registry, PluginLoader loader);

    LoadSummary initialize(const std::vector<nlohmann::json>& strategiesConfig);
    std::vector<StrategyInfo> listStrategies() const;

private:
    Config m_config;
    strategy::StrategyRegistry& m_registry;
    PluginLoader m_loader;
    std::vector<StrategyInfo> m_info;
};

} // namespace catalog
