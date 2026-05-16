#include "catalog/plugin_loader.h"

#include "logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <openssl/err.h>
#include <openssl/evp.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace catalog {

namespace {

std::filesystem::path executableDirectory() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
#else
    std::array<char, 4096> buffer{};
    const ssize_t count = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (count <= 0) {
        return {};
    }
    buffer[static_cast<size_t>(count)] = '\0';
    return std::filesystem::path(buffer.data()).parent_path();
#endif
}

std::filesystem::path normalizePluginsDir(const std::filesystem::path& configuredPath) {
    std::error_code ec;
    std::filesystem::path candidate = configuredPath.empty() ? std::filesystem::path("plugins") : configuredPath;
    if (candidate.is_relative()) {
        const auto baseDir = executableDirectory();
        if (baseDir.empty()) {
            return {};
        }
        candidate = baseDir / candidate;
    }
    const auto normalized = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
        return candidate.lexically_normal();
    }
    return normalized;
}

std::filesystem::path normalizeOptionalPath(
    const std::filesystem::path& configuredPath,
    const std::filesystem::path& baseDir) {
    if (configuredPath.empty()) {
        return {};
    }

    std::filesystem::path candidate = configuredPath;
    if (candidate.is_relative()) {
        if (baseDir.empty()) {
            return {};
        }
        candidate = baseDir / candidate;
    }

    std::error_code ec;
    const auto normalized = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
        return candidate.lexically_normal();
    }
    return normalized;
}

bool isWithinDirectory(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *rootIt != *candidateIt) {
            return false;
        }
    }
    return true;
}

std::string trim(std::string_view input) {
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])) != 0) {
        ++start;
    }
    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return std::string(input.substr(start, end - start));
}

bool isHexSha256(std::string_view value) {
    if (value.size() != 64) {
        return false;
    }
    for (char ch : value) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!std::isxdigit(uch)) {
            return false;
        }
    }
    return true;
}

std::string toLowerHex(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::string opensslError() {
    const unsigned long code = ERR_get_error();
    if (code == 0) {
        return "unknown OpenSSL error";
    }
    std::array<char, 256> buffer{};
    ERR_error_string_n(code, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

} // namespace

PluginLoader::PluginLoader(Config config, EnumerateFn enumerateFn, LoadFn loadFn)
    : m_config(std::move(config)),
      m_enumerateFn(enumerateFn ? std::move(enumerateFn) : defaultEnumerate),
      m_loadFn(loadFn ? std::move(loadFn) : defaultLoad) {
    const auto baseDir = executableDirectory();
    m_config.pluginsDir = normalizePluginsDir(m_config.pluginsDir);
    m_config.sha256AllowlistFile = normalizeOptionalPath(m_config.sha256AllowlistFile, baseDir);

    if (!m_config.enforceSha256Allowlist) {
        return;
    }
    if (m_config.sha256AllowlistFile.empty()) {
        m_allowlistLoadError = "catalog integrity is enabled but sha256 allowlist file path is empty";
        return;
    }
    auto loadedAllowlist = loadSha256Allowlist(m_config.sha256AllowlistFile);
    if (!loadedAllowlist) {
        m_allowlistLoadError = loadedAllowlist.error();
        return;
    }
    m_sha256Allowlist = std::move(*loadedAllowlist);
}

std::vector<PluginLoadResult> PluginLoader::loadAll() {
    m_results.clear();
    m_handles.clear();

    const auto candidates = m_enumerateFn(m_config.pluginsDir);
    for (const auto& path : candidates) {
        PluginLoadResult result;
        result.path = path;
        if (auto integrity = verifyIntegrity(path); !integrity) {
            result.success = false;
            result.error = integrity.error();
            m_results.push_back(std::move(result));
            continue;
        }
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
    if (it == m_handles.end() || !it->hasFactory()) {
        return {nullptr, &noopDestroy};
    }

    strategy::IStrategy* instance = nullptr;
    try {
        instance = it->create(configJson);
    } catch (const std::exception& e) {
        Logger::instance().log(
            LogLevel::Error,
            "catalog plugin factory exception type=" + std::string(strategyType) + " error=" + e.what());
        return {nullptr, &noopDestroy};
    } catch (...) {
        Logger::instance().log(
            LogLevel::Error,
            "catalog plugin factory unknown exception type=" + std::string(strategyType));
        return {nullptr, &noopDestroy};
    }
    if (!instance) {
        return {nullptr, &noopDestroy};
    }

    return {instance, it->destroyFunction()};
}

std::vector<std::filesystem::path> PluginLoader::defaultEnumerate(const std::filesystem::path& pluginsDir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (pluginsDir.empty() || !std::filesystem::exists(pluginsDir, ec) || ec) {
        return out;
    }
    const auto canonicalPluginsDir = std::filesystem::weakly_canonical(pluginsDir, ec);
    if (ec) {
        return out;
    }

    std::filesystem::directory_iterator it(pluginsDir, ec);
    std::filesystem::directory_iterator end;
    if (ec) {
        return out;
    }
    while (it != end) {
        const auto& entry = *it;
        if (!entry.is_regular_file()) {
            it.increment(ec);
            if (ec) {
                break;
            }
            continue;
        }
        const auto ext = entry.path().extension().string();
#if defined(_WIN32)
        if (ext == ".dll" || ext == ".DLL")
#else
        if (ext == ".so" || ext == ".dylib")
#endif
        {
            const auto candidatePath = std::filesystem::weakly_canonical(entry.path(), ec);
            if (!ec && isWithinDirectory(canonicalPluginsDir, candidatePath)) {
                out.push_back(candidatePath);
            }
        }
        ec.clear();
        it.increment(ec);
        if (ec) {
            break;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::expected<PluginHandle, std::string> PluginLoader::defaultLoad(const std::filesystem::path& path) {
    return PluginHandle::load(path);
}

std::expected<PluginLoader::HashAllowlist, std::string> PluginLoader::loadSha256Allowlist(
    const std::filesystem::path& allowlistPath) {
    std::ifstream input(allowlistPath);
    if (!input) {
        return std::unexpected("failed to open sha256 allowlist file: " + allowlistPath.string());
    }

    HashAllowlist hashes;
    std::string line;
    int lineNo = 0;
    while (std::getline(input, line)) {
        ++lineNo;
        const auto cleaned = trim(line);
        if (cleaned.empty() || cleaned[0] == '#') {
            continue;
        }

        std::istringstream iss(cleaned);
        std::string hashToken;
        iss >> hashToken;
        if (!isHexSha256(hashToken)) {
            return std::unexpected(
                "invalid sha256 allowlist entry at line " + std::to_string(lineNo) + ": " + cleaned);
        }
        hashes.insert(toLowerHex(hashToken));
    }
    if (hashes.empty()) {
        return std::unexpected("sha256 allowlist file is empty: " + allowlistPath.string());
    }
    return hashes;
}

std::expected<std::string, std::string> PluginLoader::calculateSha256(const std::filesystem::path& filePath) {
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        return std::unexpected("failed to open plugin for hashing: " + filePath.string());
    }

    EVP_MD_CTX* rawCtx = EVP_MD_CTX_new();
    if (!rawCtx) {
        return std::unexpected("failed to allocate digest context");
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(rawCtx, &EVP_MD_CTX_free);

    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        return std::unexpected("failed to initialize sha256 digest: " + opensslError());
    }

    std::array<char, 8192> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytesRead = input.gcount();
        if (bytesRead > 0 &&
            EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<size_t>(bytesRead)) != 1) {
            return std::unexpected("failed to update sha256 digest: " + opensslError());
        }
    }
    if (!input.eof()) {
        return std::unexpected("failed while reading plugin for hashing: " + filePath.string());
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLen = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &digestLen) != 1) {
        return std::unexpected("failed to finalize sha256 digest: " + opensslError());
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digestLen; ++i) {
        oss << std::setw(2) << static_cast<unsigned int>(digest[i]);
    }
    return oss.str();
}

std::expected<void, std::string> PluginLoader::verifyIntegrity(const std::filesystem::path& filePath) const {
    if (!m_config.enforceSha256Allowlist) {
        return {};
    }
    if (!m_allowlistLoadError.empty()) {
        return std::unexpected(m_allowlistLoadError);
    }
    auto computedHash = calculateSha256(filePath);
    if (!computedHash) {
        return std::unexpected(computedHash.error());
    }
    if (m_sha256Allowlist.find(*computedHash) == m_sha256Allowlist.end()) {
        return std::unexpected("sha256 not allowlisted for plugin: " + filePath.string());
    }
    return {};
}

void PluginLoader::noopDestroy(strategy::IStrategy*) {}

} // namespace catalog

