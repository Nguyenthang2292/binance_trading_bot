#pragma once

#include "catalog/plugin_handle.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace catalog {

struct PluginLoadResult {
    std::filesystem::path path;
    std::string type;
    std::string version;
    bool success{false};
    std::string error;
};

class PluginLoader {
public:
    struct Config {
        std::filesystem::path pluginsDir{"plugins"};
    };

    using EnumerateFn = std::function<std::vector<std::filesystem::path>(const std::filesystem::path&)>;
    using LoadFn = std::function<std::expected<PluginHandle, std::string>(const std::filesystem::path&)>;

    explicit PluginLoader(Config config, EnumerateFn enumerateFn = {}, LoadFn loadFn = {});

    std::vector<PluginLoadResult> loadAll();
    std::unique_ptr<strategy::IStrategy, void (*)(strategy::IStrategy*)> createStrategy(
        std::string_view strategyType,
        const char* configJson);

    const std::vector<PluginLoadResult>& loaded() const { return m_results; }

private:
    static std::vector<std::filesystem::path> defaultEnumerate(const std::filesystem::path& pluginsDir);
    static std::expected<PluginHandle, std::string> defaultLoad(const std::filesystem::path& path);
    static void noopDestroy(strategy::IStrategy*);

    Config m_config;
    EnumerateFn m_enumerateFn;
    LoadFn m_loadFn;
    std::vector<PluginHandle> m_handles;
    std::vector<PluginLoadResult> m_results;
};

} // namespace catalog
