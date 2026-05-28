#include "catalog/plugin_handle.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <system_error>

namespace catalog {

namespace {

void unloadHandle(void* handle) {
    if (!handle) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

void* loadSymbol(void* handle, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

std::string windowsErrorMessage(DWORD code) {
#if defined(_WIN32)
    return std::system_category().message(static_cast<int>(code));
#else
    (void)code;
    return {};
#endif
}

} // namespace

std::expected<PluginHandle, std::string> PluginHandle::load(const std::filesystem::path& dllPath) {
#if defined(_WIN32)
    HMODULE module = LoadLibraryExW(
        dllPath.wstring().c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    void* rawHandle = reinterpret_cast<void*>(module);
#else
    void* rawHandle = dlopen(dllPath.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (!rawHandle) {
#if defined(_WIN32)
        const DWORD lastError = GetLastError();
        return std::unexpected("failed to load library: " + windowsErrorMessage(lastError));
#else
        const char* err = dlerror();
        return std::unexpected(std::string("failed to load library: ") + (err ? err : "unknown error"));
#endif
    }

    PluginHandle out(rawHandle);
    out.m_path = dllPath;
    out.m_createFn = reinterpret_cast<CreateFn>(loadSymbol(rawHandle, "createStrategy"));
    out.m_destroyFn = reinterpret_cast<DestroyFn>(loadSymbol(rawHandle, "destroyStrategy"));
    out.m_typeFn = reinterpret_cast<TypeFn>(loadSymbol(rawHandle, "strategyType"));
    out.m_versionFn = reinterpret_cast<VersionFn>(loadSymbol(rawHandle, "pluginVersion"));
    if (!out.m_createFn || !out.m_destroyFn || !out.m_typeFn || !out.m_versionFn) {
        unloadHandle(rawHandle);
        return std::unexpected("missing required exports");
    }

    const char* type = out.m_typeFn();
    const char* version = out.m_versionFn();
    if (!type || !version) {
        unloadHandle(rawHandle);
        return std::unexpected("invalid plugin metadata");
    }
    out.m_type = type;
    out.m_version = version;
    return out;
}

PluginHandle PluginHandle::fromExports(
    std::filesystem::path path,
    CreateFn createFnValue,
    DestroyFn destroyFnValue,
    TypeFn typeFnValue,
    VersionFn versionFnValue,
    std::string typeValue,
    std::string versionValue) {
    PluginHandle out;
    out.m_path = std::move(path);
    out.m_type = std::move(typeValue);
    out.m_version = std::move(versionValue);
    out.m_createFn = createFnValue;
    out.m_destroyFn = destroyFnValue;
    out.m_typeFn = typeFnValue;
    out.m_versionFn = versionFnValue;
    return out;
}

PluginHandle::PluginHandle(PluginHandle&& other) noexcept
    : m_path(std::move(other.m_path)),
      m_type(std::move(other.m_type)),
      m_version(std::move(other.m_version)),
      m_createFn(other.m_createFn),
      m_destroyFn(other.m_destroyFn),
      m_typeFn(other.m_typeFn),
      m_versionFn(other.m_versionFn),
      m_handle(other.m_handle) {
    other.m_handle = nullptr;
    other.m_createFn = nullptr;
    other.m_destroyFn = nullptr;
    other.m_typeFn = nullptr;
    other.m_versionFn = nullptr;
}

PluginHandle& PluginHandle::operator=(PluginHandle&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    unloadHandle(m_handle);
    m_createFn = other.m_createFn;
    m_destroyFn = other.m_destroyFn;
    m_typeFn = other.m_typeFn;
    m_versionFn = other.m_versionFn;
    m_path = std::move(other.m_path);
    m_type = std::move(other.m_type);
    m_version = std::move(other.m_version);
    m_handle = other.m_handle;
    other.m_handle = nullptr;
    other.m_createFn = nullptr;
    other.m_destroyFn = nullptr;
    other.m_typeFn = nullptr;
    other.m_versionFn = nullptr;
    return *this;
}

PluginHandle::~PluginHandle() {
    unloadHandle(m_handle);
}

const std::filesystem::path& PluginHandle::path() const noexcept {
    return m_path;
}

std::string_view PluginHandle::type() const noexcept {
    return m_type;
}

std::string_view PluginHandle::version() const noexcept {
    return m_version;
}

bool PluginHandle::hasFactory() const {
    return m_createFn != nullptr && m_destroyFn != nullptr;
}

strategy::IStrategy* PluginHandle::create(const char* configJson) const {
    if (!m_createFn) {
        return nullptr;
    }
    return m_createFn(configJson);
}

PluginHandle::DestroyFn PluginHandle::destroyFunction() const {
    return m_destroyFn;
}

} // namespace catalog

