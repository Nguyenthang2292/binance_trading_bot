#include "catalog/plugin_loader.h"

#include "logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <exception>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_set>

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
    const auto componentEquals = [](const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
#if defined(_WIN32)
        const auto lhsNative = lhs.native();
        const auto rhsNative = rhs.native();
        if (lhsNative.size() != rhsNative.size()) {
            return false;
        }
        for (size_t i = 0; i < lhsNative.size(); ++i) {
            if (std::towlower(lhsNative[i]) != std::towlower(rhsNative[i])) {
                return false;
            }
        }
        return true;
#else
        return lhs == rhs;
#endif
    };

    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || !componentEquals(*rootIt, *candidateIt)) {
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
    Logger::instance().log(
        LogLevel::Warning,
        "catalog integrity note: SHA-256 allowlist checks bytes before load; keep plugins directory write-restricted");
}

std::vector<PluginLoadResult> PluginLoader::loadAll() {
    m_results.clear();
    m_handles.clear();
    std::unordered_set<std::string> loadedTypes;

    const auto candidates = m_enumerateFn(m_config.pluginsDir);
    for (const auto& path : candidates) {
        PluginLoadResult result;
        result.path = path;
        std::optional<std::string> preLoadHash;
        if (auto integrity = verifyIntegrity(path); !integrity) {
            result.success = false;
            result.error = integrity.error();
            m_results.push_back(std::move(result));
            continue;
        } else if (!integrity->empty()) {
            preLoadHash = *integrity;
        }
        auto loaded = m_loadFn(path);
        if (!loaded) {
            result.success = false;
            result.error = loaded.error();
            m_results.push_back(std::move(result));
            continue;
        }
        if (preLoadHash.has_value()) {
            auto postLoadHash = calculateSha256(path);
            if (!postLoadHash) {
                result.success = false;
                result.error = postLoadHash.error();
                m_results.push_back(std::move(result));
                continue;
            }
            if (*postLoadHash != *preLoadHash) {
                result.success = false;
                result.error = "plugin changed between integrity check and load: " + path.string();
                m_results.push_back(std::move(result));
                continue;
            }
        }

        result.success = true;
        result.type = std::string(loaded->type());
        result.version = std::string(loaded->version());
        result.abiVersion = loaded->abiVersion();
        if (!loadedTypes.insert(result.type).second) {
            result.success = false;
            result.error = "duplicate strategy type exported by plugins: " + result.type;
            m_results.push_back(std::move(result));
            continue;
        }
        m_handles.push_back(std::make_shared<PluginHandle>(std::move(*loaded)));
        m_results.push_back(std::move(result));
    }
    return m_results;
}

std::shared_ptr<strategy::IStrategy> PluginLoader::createStrategy(
    std::string_view strategyType,
    const char* configJson) {
    const auto it = std::find_if(
        m_handles.begin(),
        m_handles.end(),
        [strategyType](const std::shared_ptr<PluginHandle>& handle) {
            return handle && handle->type() == strategyType;
        });
    if (it == m_handles.end() || !(*it) || !(*it)->hasFactory()) {
        return {};
    }
    const auto lease = *it;

    strategy::IStrategy* instance = nullptr;
    try {
        instance = lease->create(configJson);
    } catch (const std::exception& e) {
        Logger::instance().log(
            LogLevel::Error,
            "catalog plugin factory exception type=" + std::string(strategyType) + " error=" + e.what());
        return {};
    } catch (...) {
        Logger::instance().log(
            LogLevel::Error,
            "catalog plugin factory unknown exception type=" + std::string(strategyType));
        return {};
    }
    if (!instance) {
        return {};
    }
    const auto destroy = lease->destroyFunction();
    if (!destroy) {
        return {};
    }
    return std::shared_ptr<strategy::IStrategy>(
        instance,
        [lease, destroy](strategy::IStrategy* ptr) {
            if (ptr) {
                destroy(ptr);
            }
        });
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
        auto ext = entry.path().extension().string();
#if defined(_WIN32)
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (ext == ".dll")
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

compat::expected<PluginHandle, std::string> PluginLoader::defaultLoad(const std::filesystem::path& path) {
    return PluginHandle::load(path);
}

compat::expected<PluginLoader::HashAllowlist, std::string> PluginLoader::loadSha256Allowlist(
    const std::filesystem::path& allowlistPath) {
    std::ifstream input(allowlistPath);
    if (!input) {
        return compat::unexpected("failed to open sha256 allowlist file: " + allowlistPath.string());
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
            return compat::unexpected(
                "invalid sha256 allowlist entry at line " + std::to_string(lineNo) + ": " + cleaned);
        }
        hashes.insert(toLowerHex(hashToken));
    }
    if (hashes.empty()) {
        return compat::unexpected("sha256 allowlist file is empty: " + allowlistPath.string());
    }
    return hashes;
}

compat::expected<std::string, std::string> PluginLoader::calculateSha256(const std::filesystem::path& filePath) {
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        return compat::unexpected("failed to open plugin for hashing: " + filePath.string());
    }

    EVP_MD_CTX* rawCtx = EVP_MD_CTX_new();
    if (!rawCtx) {
        return compat::unexpected("failed to allocate digest context");
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(rawCtx, &EVP_MD_CTX_free);

    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        return compat::unexpected("failed to initialize sha256 digest: " + opensslError());
    }

    std::array<char, 8192> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytesRead = input.gcount();
        if (bytesRead > 0 &&
            EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<size_t>(bytesRead)) != 1) {
            return compat::unexpected("failed to update sha256 digest: " + opensslError());
        }
    }
    if (!input.eof()) {
        return compat::unexpected("failed while reading plugin for hashing: " + filePath.string());
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLen = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &digestLen) != 1) {
        return compat::unexpected("failed to finalize sha256 digest: " + opensslError());
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digestLen; ++i) {
        oss << std::setw(2) << static_cast<unsigned int>(digest[i]);
    }
    return oss.str();
}

compat::expected<std::string, std::string> PluginLoader::verifyIntegrity(const std::filesystem::path& filePath) const {
    if (!m_config.enforceSha256Allowlist) {
        return std::string{};
    }
    if (!m_allowlistLoadError.empty()) {
        return compat::unexpected(m_allowlistLoadError);
    }
    auto computedHash = calculateSha256(filePath);
    if (!computedHash) {
        return compat::unexpected(computedHash.error());
    }
    if (m_sha256Allowlist.find(*computedHash) == m_sha256Allowlist.end()) {
        return compat::unexpected("sha256 not allowlisted for plugin: " + filePath.string());
    }
    return *computedHash;
}

} // namespace catalog

