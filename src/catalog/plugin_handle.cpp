#include "catalog/plugin_handle.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

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

} // namespace

std::expected<PluginHandle, std::string> PluginHandle::load(const std::filesystem::path& dllPath) {
#if defined(_WIN32)
    HMODULE module = LoadLibraryA(dllPath.string().c_str());
    void* rawHandle = reinterpret_cast<void*>(module);
#else
    void* rawHandle = dlopen(dllPath.string().c_str(), RTLD_NOW);
#endif
    if (!rawHandle) {
        return std::unexpected("failed to load library");
    }

    PluginHandle out(rawHandle);
    out.path = dllPath;
    out.createFn = reinterpret_cast<CreateFn>(loadSymbol(rawHandle, "createStrategy"));
    out.destroyFn = reinterpret_cast<DestroyFn>(loadSymbol(rawHandle, "destroyStrategy"));
    out.typeFn = reinterpret_cast<TypeFn>(loadSymbol(rawHandle, "strategyType"));
    out.versionFn = reinterpret_cast<VersionFn>(loadSymbol(rawHandle, "pluginVersion"));
    if (!out.createFn || !out.destroyFn || !out.typeFn || !out.versionFn) {
        unloadHandle(rawHandle);
        return std::unexpected("missing required exports");
    }

    const char* type = out.typeFn();
    const char* version = out.versionFn();
    if (!type || !version) {
        unloadHandle(rawHandle);
        return std::unexpected("invalid plugin metadata");
    }
    out.type = type;
    out.version = version;
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
    out.path = std::move(path);
    out.createFn = createFnValue;
    out.destroyFn = destroyFnValue;
    out.typeFn = typeFnValue;
    out.versionFn = versionFnValue;
    out.type = std::move(typeValue);
    out.version = std::move(versionValue);
    return out;
}

PluginHandle::PluginHandle(PluginHandle&& other) noexcept
    : createFn(other.createFn),
      destroyFn(other.destroyFn),
      typeFn(other.typeFn),
      versionFn(other.versionFn),
      path(std::move(other.path)),
      type(std::move(other.type)),
      version(std::move(other.version)),
      m_handle(other.m_handle) {
    other.m_handle = nullptr;
    other.createFn = nullptr;
    other.destroyFn = nullptr;
    other.typeFn = nullptr;
    other.versionFn = nullptr;
}

PluginHandle& PluginHandle::operator=(PluginHandle&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    unloadHandle(m_handle);
    createFn = other.createFn;
    destroyFn = other.destroyFn;
    typeFn = other.typeFn;
    versionFn = other.versionFn;
    path = std::move(other.path);
    type = std::move(other.type);
    version = std::move(other.version);
    m_handle = other.m_handle;
    other.m_handle = nullptr;
    other.createFn = nullptr;
    other.destroyFn = nullptr;
    other.typeFn = nullptr;
    other.versionFn = nullptr;
    return *this;
}

PluginHandle::~PluginHandle() {
    unloadHandle(m_handle);
}

} // namespace catalog

