#include "catalog/strategy_catalog.h"

#include <algorithm>

namespace catalog {

StrategyCatalog::StrategyCatalog(Config config, strategy::StrategyRegistry& registry, PluginLoader loader)
    : m_config(std::move(config)), m_registry(registry), m_loader(std::move(loader)) {}

StrategyCatalog::StrategyCatalog(Config config, strategy::StrategyRegistry& registry)
    : m_config(std::move(config)),
      m_registry(registry),
      m_loader(PluginLoader::Config{
          .pluginsDir = m_config.pluginsDir,
          .enforceSha256Allowlist = m_config.enforceSha256Allowlist,
          .sha256AllowlistFile = m_config.sha256AllowlistFile}) {}

StrategyCatalog::LoadSummary StrategyCatalog::initialize(const std::vector<nlohmann::json>& strategiesConfig) {
    m_info.clear();
    m_registry.clear();
    LoadSummary summary;
    const auto pluginResults = m_loader.loadAll();
    summary.pluginsFound = static_cast<int>(pluginResults.size());
    for (const auto& result : pluginResults) {
        if (result.success) {
            ++summary.pluginsLoaded;
        } else {
            summary.errors.push_back(result.path.string() + ": " + result.error);
        }
    }

    for (const auto& item : strategiesConfig) {
        if (!item.is_object()) {
            summary.errors.push_back("strategy config must be an object");
            continue;
        }
        if (!item.contains("type")) {
            summary.errors.push_back("strategy config missing type");
            continue;
        }
        if (!item.at("type").is_string()) {
            summary.errors.push_back("strategy config 'type' must be a string");
            continue;
        }
        const std::string type = item.at("type").get<std::string>();
        if (type.empty()) {
            summary.errors.push_back("strategy config missing type");
            continue;
        }

        const std::string jsonConfig = item.dump();
        auto shared = m_loader.createStrategy(type, jsonConfig.c_str());
        if (!shared) {
            summary.errors.push_back("failed creating strategy type=" + type);
            continue;
        }
        const auto& cfg = shared->config();
        if (auto validation = strategy::validateStrategyConfig(cfg); !validation) {
            summary.errors.push_back(
                "invalid strategy config type=" + type + " reason=" + validation.error());
            continue;
        }

        StrategyInfo info;
        info.name = cfg.name;
        info.type = type;
        info.intervals = cfg.intervals;
        info.scanInterval = cfg.scanInterval;
        info.maxHoldDuration = cfg.maxHoldDuration;

        const auto loadedIt = std::find_if(
            pluginResults.begin(),
            pluginResults.end(),
            [&type](const PluginLoadResult& result) { return result.success && result.type == type; });
        if (loadedIt != pluginResults.end()) {
            info.version = loadedIt->version;
            info.pluginFile = loadedIt->path.filename().string();
        }

        m_registry.addShared(shared);
        m_info.push_back(std::move(info));
        ++summary.strategiesRegistered;
    }

    return summary;
}

std::vector<StrategyCatalog::StrategyInfo> StrategyCatalog::listStrategies() const {
    return m_info;
}

} // namespace catalog
