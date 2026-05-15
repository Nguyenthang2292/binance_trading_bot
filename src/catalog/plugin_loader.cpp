#include "catalog/plugin_loader.h"

#include <algorithm>

namespace catalog {

PluginLoader::PluginLoader(Config config, EnumerateFn enumerateFn, LoadFn loadFn)
    : m_config(std::move(config)),
      m_enumerateFn(enumerateFn ? std::move(enumerateFn) : defaultEnumerate),
      m_loadFn(loadFn ? std::move(loadFn) : defaultLoad) {}

std::vector<PluginLoadResult> PluginLoader::loadAll() {
    m_results.clear();
    m_handles.clear();

    const auto candidates = m_enumerateFn(m_config.pluginsDir);
    for (const auto& path : candidates) {
        PluginLoadResult result;
        result.path = path;
        auto loaded = m_loadFn(path);
        if (!loaded) {
            result.success = false;
            result.error = loaded.error();
            m_results.push_back(std::move(result));
            continue;
        }

        result.success = true;
        result.type = loaded->type;
        result.version = loaded->version;
        m_handles.push_back(std::move(*loaded));
        m_results.push_back(std::move(result));
    }
    return m_results;
}

std::unique_ptr<strategy::IStrategy, void (*)(strategy::IStrategy*)> PluginLoader::createStrategy(
    std::string_view strategyType,
    const char* configJson) {
    const auto it = std::find_if(m_handles.begin(), m_handles.end(), [strategyType](const PluginHandle& handle) {
        return handle.type == strategyType;
    });
    if (it == m_handles.end() || !it->createFn || !it->destroyFn) {
        return {nullptr, &noopDestroy};
    }

    strategy::IStrategy* instance = nullptr;
    try {
        instance = it->createFn(configJson);
    } catch (...) {
        return {nullptr, &noopDestroy};
    }
    if (!instance) {
        return {nullptr, &noopDestroy};
    }

    return {instance, it->destroyFn};
}

std::vector<std::filesystem::path> PluginLoader::defaultEnumerate(const std::filesystem::path& pluginsDir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (!std::filesystem::exists(pluginsDir, ec) || ec) {
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(pluginsDir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto ext = entry.path().extension().string();
#if defined(_WIN32)
        if (ext == ".dll" || ext == ".DLL")
#else
        if (ext == ".so" || ext == ".dylib")
#endif
        {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::expected<PluginHandle, std::string> PluginLoader::defaultLoad(const std::filesystem::path& path) {
    return PluginHandle::load(path);
}

void PluginLoader::noopDestroy(strategy::IStrategy*) {}

} // namespace catalog

