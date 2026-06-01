#pragma once

#include "strategy/istrategy.h"

#include "common/expected_compat.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace catalog {

class PluginHandle {
public:
    using CreateFn = strategy::IStrategy* (*)(const char*);
    using DestroyFn = void (*)(strategy::IStrategy*);
    using TypeFn = const char* (*)();
    using VersionFn = const char* (*)();
    using AbiVersionFn = int (*)();

    static compat::expected<PluginHandle, std::string> load(const std::filesystem::path& dllPath);
    static PluginHandle fromExports(
        std::filesystem::path path,
        CreateFn createFn,
        DestroyFn destroyFn,
        TypeFn typeFn,
        VersionFn versionFn,
        std::string type,
        std::string version,
        int abiVersion = strategy::kStrategyPluginAbiVersion);

    PluginHandle() = default;
    PluginHandle(PluginHandle&& other) noexcept;
    PluginHandle& operator=(PluginHandle&& other) noexcept;
    PluginHandle(const PluginHandle&) = delete;
    PluginHandle& operator=(const PluginHandle&) = delete;
    ~PluginHandle();

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] std::string_view type() const noexcept;
    [[nodiscard]] std::string_view version() const noexcept;
    [[nodiscard]] int abiVersion() const noexcept;
    [[nodiscard]] bool hasFactory() const;
    [[nodiscard]] strategy::IStrategy* create(const char* configJson) const;
    [[nodiscard]] DestroyFn destroyFunction() const;

private:
    explicit PluginHandle(void* handle) : m_handle(handle) {}
    std::filesystem::path m_path;
    std::string m_type;
    std::string m_version;
    int m_abiVersion{0};
    CreateFn m_createFn{nullptr};
    DestroyFn m_destroyFn{nullptr};
    TypeFn m_typeFn{nullptr};
    VersionFn m_versionFn{nullptr};
    void* m_handle{nullptr};
};

} // namespace catalog

