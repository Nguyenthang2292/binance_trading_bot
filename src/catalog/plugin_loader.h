#pragma once

#include "catalog/plugin_handle.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace catalog {

struct PluginLoadResult {
    std::filesystem::path path;
    std::string type;
    std::string version;
    int abiVersion{0};
    bool success{false};
    std::string error;
};

class PluginLoader {
public:
    struct Config {
        std::filesystem::path pluginsDir{"plugins"};
        bool enforceSha256Allowlist{false};
        // Integrity check hashes the on-disk file before dynamic loading. Keep plugin directory ACLs strict.
        std::filesystem::path sha256AllowlistFile{};
    };

    using EnumerateFn = std::function<std::vector<std::filesystem::path>(const std::filesystem::path&)>;
    using LoadFn = std::function<compat::expected<PluginHandle, std::string>(const std::filesystem::path&)>;

    explicit PluginLoader(Config config, EnumerateFn enumerateFn = {}, LoadFn loadFn = {});

    // Reloads plugin handles. Strategy instances keep a shared lease to the
    // originating handle so code is not unloaded while instances are alive.
    std::vector<PluginLoadResult> loadAll();
    std::shared_ptr<strategy::IStrategy> createStrategy(
        std::string_view strategyType,
        const char* configJson);

    const std::vector<PluginLoadResult>& loaded() const { return m_results; }

private:
    using HashAllowlist = std::unordered_set<std::string>;

    static std::vector<std::filesystem::path> defaultEnumerate(const std::filesystem::path& pluginsDir);
    static compat::expected<PluginHandle, std::string> defaultLoad(const std::filesystem::path& path);
    static compat::expected<HashAllowlist, std::string> loadSha256Allowlist(const std::filesystem::path& allowlistPath);
    static compat::expected<std::string, std::string> calculateSha256(const std::filesystem::path& filePath);
    compat::expected<std::string, std::string> verifyIntegrity(const std::filesystem::path& filePath) const;

    Config m_config;
    EnumerateFn m_enumerateFn;
    LoadFn m_loadFn;
    std::vector<std::shared_ptr<PluginHandle>> m_handles;
    std::vector<PluginLoadResult> m_results;
    HashAllowlist m_sha256Allowlist;
    std::string m_allowlistLoadError;
};

} // namespace catalog
