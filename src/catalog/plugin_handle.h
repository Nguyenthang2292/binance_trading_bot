#pragma once

#include "strategy/istrategy.h"

#include "common/expected_compat.h"

#include <filesystem>
#include <string>

namespace catalog {

class PluginHandle {
public:
    using CreateFn = strategy::IStrategy* (*)(const char*);
    using DestroyFn = void (*)(strategy::IStrategy*);
    using TypeFn = const char* (*)();
    using VersionFn = const char* (*)();

    static std::expected<PluginHandle, std::string> load(const std::filesystem::path& dllPath);
    static PluginHandle fromExports(
        std::filesystem::path path,
        CreateFn createFn,
        DestroyFn destroyFn,
        TypeFn typeFn,
        VersionFn versionFn,
        std::string type,
        std::string version);

    PluginHandle() = default;
    PluginHandle(PluginHandle&& other) noexcept;
    PluginHandle& operator=(PluginHandle&& other) noexcept;
    PluginHandle(const PluginHandle&) = delete;
    PluginHandle& operator=(const PluginHandle&) = delete;
    ~PluginHandle();

    CreateFn createFn{nullptr};
    DestroyFn destroyFn{nullptr};
    TypeFn typeFn{nullptr};
    VersionFn versionFn{nullptr};

    std::filesystem::path path;
    std::string type;
    std::string version;

private:
    explicit PluginHandle(void* handle) : m_handle(handle) {}
    void* m_handle{nullptr};
};

} // namespace catalog

