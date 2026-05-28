#include <gtest/gtest.h>

#include "catalog/plugin_loader.h"

#include <chrono>
#include <fstream>
#include <stdexcept>

namespace {

class DummyStrategy final : public strategy::IStrategy {
public:
    explicit DummyStrategy(strategy::StrategyConfig cfg) : m_cfg(std::move(cfg)) {}
    const strategy::StrategyConfig& config() const override { return m_cfg; }
    strategy::Signal evaluate(std::string_view, std::string_view, const std::vector<Kline>&) const override {
        return {};
    }

private:
    strategy::StrategyConfig m_cfg;
};

strategy::IStrategy* createOk(const char*) {
    strategy::StrategyConfig cfg;
    cfg.name = "dummy";
    return new DummyStrategy(cfg);
}
strategy::IStrategy* createNull(const char*) { return nullptr; }
strategy::IStrategy* createThrowsStd(const char*) { throw std::runtime_error("factory failed"); }
strategy::IStrategy* createThrowsUnknown(const char*) { throw 42; }
void destroyOk(strategy::IStrategy* ptr) { delete ptr; }
const char* typeFn() { return "dummy"; }
const char* verFn() { return "1.0.0"; }

struct TempDirGuard {
    std::filesystem::path path;
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::filesystem::path makeTempDir() {
    std::error_code ec;
    const auto base = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return {};
    }
    const auto suffix = std::to_string(
        static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = base / ("plugin-loader-" + suffix);
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return {};
    }
    return path;
}

void writeTextFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

} // namespace

TEST(PluginLoaderTest, LoadAllAndCreateSuccess) {
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins"},
        [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{"a.dll"}; },
        [](const std::filesystem::path& path) {
            return std::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path, &createOk, &destroyOk, &typeFn, &verFn, "dummy", "1.0.0"));
        });

    const auto results = loader.loadAll();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].success);
    EXPECT_EQ(results[0].type, "dummy");

    auto instance = loader.createStrategy("dummy", "{}");
    ASSERT_TRUE(instance);
    EXPECT_EQ(instance->config().name, "dummy");
}

TEST(PluginLoaderTest, CapturesMissingExportFailure) {
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins"},
        [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{"bad.dll"}; },
        [](const std::filesystem::path&) {
            return std::expected<catalog::PluginHandle, std::string>(
                std::unexpected(std::string("missing required exports")));
        });

    const auto results = loader.loadAll();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].success);
    EXPECT_NE(results[0].error.find("missing"), std::string::npos);
}

TEST(PluginLoaderTest, CreateReturnsNullWhenFactoryReturnsNullptr) {
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins"},
        [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{"null.dll"}; },
        [](const std::filesystem::path& path) {
            return std::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path, &createNull, &destroyOk, &typeFn, &verFn, "dummy", "1.0.0"));
        });

    (void)loader.loadAll();
    auto instance = loader.createStrategy("dummy", "{}");
    EXPECT_FALSE(instance);
}

TEST(PluginLoaderTest, EnforcedSha256AllowlistBlocksUnlistedPlugin) {
    const auto tmpDir = makeTempDir();
    ASSERT_FALSE(tmpDir.empty());
    TempDirGuard cleanup{tmpDir};

    const auto pluginPath = tmpDir / "plugin.dll";
    const auto allowlistPath = tmpDir / "allowlist.txt";
    writeTextFile(pluginPath, "abc");
    writeTextFile(allowlistPath, "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef\n");

    int loadCalls = 0;
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins", .enforceSha256Allowlist = true, .sha256AllowlistFile = allowlistPath},
        [pluginPath](const std::filesystem::path&) { return std::vector<std::filesystem::path>{pluginPath}; },
        [&loadCalls](const std::filesystem::path& path) {
            ++loadCalls;
            return std::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path, &createOk, &destroyOk, &typeFn, &verFn, "dummy", "1.0.0"));
        });

    const auto results = loader.loadAll();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].success);
    EXPECT_NE(results[0].error.find("sha256 not allowlisted"), std::string::npos);
    EXPECT_EQ(loadCalls, 0);
}

TEST(PluginLoaderTest, EnforcedSha256AllowlistLoadsAllowlistedPlugin) {
    const auto tmpDir = makeTempDir();
    ASSERT_FALSE(tmpDir.empty());
    TempDirGuard cleanup{tmpDir};

    const auto pluginPath = tmpDir / "plugin.dll";
    const auto allowlistPath = tmpDir / "allowlist.txt";
    writeTextFile(pluginPath, "abc");
    // SHA-256("abc")
    writeTextFile(allowlistPath, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n");

    int loadCalls = 0;
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins", .enforceSha256Allowlist = true, .sha256AllowlistFile = allowlistPath},
        [pluginPath](const std::filesystem::path&) { return std::vector<std::filesystem::path>{pluginPath}; },
        [&loadCalls](const std::filesystem::path& path) {
            ++loadCalls;
            return std::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path, &createOk, &destroyOk, &typeFn, &verFn, "dummy", "1.0.0"));
        });

    const auto results = loader.loadAll();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].success);
    EXPECT_EQ(loadCalls, 1);
}

TEST(PluginLoaderTest, CreateReturnsNullWhenFactoryThrowsStdException) {
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins"},
        [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{"throw_std.dll"}; },
        [](const std::filesystem::path& path) {
            return std::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path, &createThrowsStd, &destroyOk, &typeFn, &verFn, "dummy", "1.0.0"));
        });

    (void)loader.loadAll();
    auto instance = loader.createStrategy("dummy", "{}");
    EXPECT_FALSE(instance);
}

TEST(PluginLoaderTest, CreateReturnsNullWhenFactoryThrowsUnknownException) {
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins"},
        [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{"throw_unknown.dll"}; },
        [](const std::filesystem::path& path) {
            return std::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path, &createThrowsUnknown, &destroyOk, &typeFn, &verFn, "dummy", "1.0.0"));
        });

    (void)loader.loadAll();
    auto instance = loader.createStrategy("dummy", "{}");
    EXPECT_FALSE(instance);
}

TEST(PluginLoaderTest, EnforcedSha256AllowlistWithEmptyPathFailsClosed) {
    const auto tmpDir = makeTempDir();
    ASSERT_FALSE(tmpDir.empty());
    TempDirGuard cleanup{tmpDir};
    const auto pluginPath = tmpDir / "plugin.dll";
    writeTextFile(pluginPath, "abc");

    int loadCalls = 0;
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins", .enforceSha256Allowlist = true},
        [pluginPath](const std::filesystem::path&) { return std::vector<std::filesystem::path>{pluginPath}; },
        [&loadCalls](const std::filesystem::path& path) {
            ++loadCalls;
            return std::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path, &createOk, &destroyOk, &typeFn, &verFn, "dummy", "1.0.0"));
        });

    const auto results = loader.loadAll();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].success);
    EXPECT_NE(results[0].error.find("allowlist file path is empty"), std::string::npos);
    EXPECT_EQ(loadCalls, 0);
}

TEST(PluginLoaderTest, EnforcedSha256AllowlistMissingFileFailsClosed) {
    const auto tmpDir = makeTempDir();
    ASSERT_FALSE(tmpDir.empty());
    TempDirGuard cleanup{tmpDir};
    const auto pluginPath = tmpDir / "plugin.dll";
    const auto allowlistPath = tmpDir / "missing_allowlist.txt";
    writeTextFile(pluginPath, "abc");

    int loadCalls = 0;
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins", .enforceSha256Allowlist = true, .sha256AllowlistFile = allowlistPath},
        [pluginPath](const std::filesystem::path&) { return std::vector<std::filesystem::path>{pluginPath}; },
        [&loadCalls](const std::filesystem::path& path) {
            ++loadCalls;
            return std::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path, &createOk, &destroyOk, &typeFn, &verFn, "dummy", "1.0.0"));
        });

    const auto results = loader.loadAll();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].success);
    EXPECT_NE(results[0].error.find("failed to open sha256 allowlist file"), std::string::npos);
    EXPECT_EQ(loadCalls, 0);
}

TEST(PluginLoaderTest, EnforcedSha256AllowlistRejectsEmptyAllowlistFile) {
    const auto tmpDir = makeTempDir();
    ASSERT_FALSE(tmpDir.empty());
    TempDirGuard cleanup{tmpDir};
    const auto pluginPath = tmpDir / "plugin.dll";
    const auto allowlistPath = tmpDir / "allowlist.txt";
    writeTextFile(pluginPath, "abc");
    writeTextFile(allowlistPath, "");

    int loadCalls = 0;
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins", .enforceSha256Allowlist = true, .sha256AllowlistFile = allowlistPath},
        [pluginPath](const std::filesystem::path&) { return std::vector<std::filesystem::path>{pluginPath}; },
        [&loadCalls](const std::filesystem::path& path) {
            ++loadCalls;
            return std::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path, &createOk, &destroyOk, &typeFn, &verFn, "dummy", "1.0.0"));
        });

    const auto results = loader.loadAll();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].success);
    EXPECT_NE(results[0].error.find("allowlist file is empty"), std::string::npos);
    EXPECT_EQ(loadCalls, 0);
}

TEST(PluginLoaderTest, EnforcedSha256AllowlistRejectsInvalidHexEntry) {
    const auto tmpDir = makeTempDir();
    ASSERT_FALSE(tmpDir.empty());
    TempDirGuard cleanup{tmpDir};
    const auto pluginPath = tmpDir / "plugin.dll";
    const auto allowlistPath = tmpDir / "allowlist.txt";
    writeTextFile(pluginPath, "abc");
    writeTextFile(allowlistPath, "not-a-valid-sha\n");

    int loadCalls = 0;
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins", .enforceSha256Allowlist = true, .sha256AllowlistFile = allowlistPath},
        [pluginPath](const std::filesystem::path&) { return std::vector<std::filesystem::path>{pluginPath}; },
        [&loadCalls](const std::filesystem::path& path) {
            ++loadCalls;
            return std::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path, &createOk, &destroyOk, &typeFn, &verFn, "dummy", "1.0.0"));
        });

    const auto results = loader.loadAll();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].success);
    EXPECT_NE(results[0].error.find("invalid sha256 allowlist entry"), std::string::npos);
    EXPECT_EQ(loadCalls, 0);
}

// T6 — normalizePluginsDir: a relative pluginsDir is resolved to an absolute path
// (anchored to the executable directory) before enumeration runs.
TEST(PluginLoaderTest, NormalizesRelativePluginsDirToAbsolute) {
    std::filesystem::path received;
    catalog::PluginLoader loader(
        {.pluginsDir = "plugins"},
        [&received](const std::filesystem::path& dir) {
            received = dir;
            return std::vector<std::filesystem::path>{};
        },
        [](const std::filesystem::path&) {
            return std::expected<catalog::PluginHandle, std::string>(
                std::unexpected(std::string("unused")));
        });

    (void)loader.loadAll();
    EXPECT_TRUE(received.is_absolute());
    EXPECT_EQ(received.filename(), std::filesystem::path("plugins"));
}

// T6 — normalizePluginsDir: an absolute pluginsDir is preserved (canonicalized), not relocated.
TEST(PluginLoaderTest, PreservesAbsolutePluginsDir) {
    const auto tmpDir = makeTempDir();
    ASSERT_FALSE(tmpDir.empty());
    TempDirGuard cleanup{tmpDir};

    std::filesystem::path received;
    catalog::PluginLoader loader(
        {.pluginsDir = tmpDir},
        [&received](const std::filesystem::path& dir) {
            received = dir;
            return std::vector<std::filesystem::path>{};
        },
        [](const std::filesystem::path&) {
            return std::expected<catalog::PluginHandle, std::string>(
                std::unexpected(std::string("unused")));
        });

    (void)loader.loadAll();

    std::error_code ec;
    const auto expected = std::filesystem::weakly_canonical(tmpDir, ec);
    ASSERT_FALSE(ec);
    EXPECT_TRUE(received.is_absolute());
    EXPECT_EQ(received, expected);
}

// T5 — defaultEnumerate path confinement: a symlink inside the plugins directory that
// resolves to a file OUTSIDE the directory must be rejected (isWithinDirectory check).
TEST(PluginLoaderTest, DefaultEnumerateRejectsSymlinkEscapingPluginsDir) {
    const auto tmpRoot = makeTempDir();
    ASSERT_FALSE(tmpRoot.empty());
    TempDirGuard cleanup{tmpRoot};

    const auto pluginsDir = tmpRoot / "plugins";
    const auto outsideDir = tmpRoot / "outside";
    std::error_code ec;
    std::filesystem::create_directories(pluginsDir, ec);
    ASSERT_FALSE(ec);
    std::filesystem::create_directories(outsideDir, ec);
    ASSERT_FALSE(ec);

#if defined(_WIN32)
    const std::string ext = ".dll";
#else
    const std::string ext = ".so";
#endif

    const auto legit = pluginsDir / ("legit" + ext);
    const auto outsideTarget = outsideDir / ("target" + ext);
    writeTextFile(legit, "legit-bytes");
    writeTextFile(outsideTarget, "evil-bytes");

    // Symlink in plugins/ pointing outside the directory. Requires privileges on Windows.
    const auto escapeLink = pluginsDir / ("escape" + ext);
    std::filesystem::create_symlink(outsideTarget, escapeLink, ec);
    if (ec) {
        GTEST_SKIP() << "symlink creation unavailable (privileges/dev-mode?): " << ec.message();
    }

    catalog::PluginLoader loader(
        {.pluginsDir = pluginsDir},
        {}, // use the real defaultEnumerate so path confinement is exercised
        [](const std::filesystem::path& path) {
            return std::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path, &createOk, &destroyOk, &typeFn, &verFn, "dummy", "1.0.0"));
        });

    const auto results = loader.loadAll();

    // Only the in-directory plugin survives; the escaping symlink is filtered out.
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].path.filename(), legit.filename());

    const auto canonicalPlugins = std::filesystem::weakly_canonical(pluginsDir, ec);
    ASSERT_FALSE(ec);
    EXPECT_EQ(results[0].path.parent_path(), canonicalPlugins);
    for (const auto& result : results) {
        EXPECT_NE(result.path.filename(), outsideTarget.filename());
    }
}

// T5 — defaultEnumerate (no privileges required): returns only plugin-extension files,
// canonicalized, sorted, and confined to the plugins directory. Exercises the positive
// branch of isWithinDirectory plus the extension filter on every platform.
TEST(PluginLoaderTest, DefaultEnumerateReturnsSortedConfinedPluginFiles) {
    const auto tmpRoot = makeTempDir();
    ASSERT_FALSE(tmpRoot.empty());
    TempDirGuard cleanup{tmpRoot};

    const auto pluginsDir = tmpRoot / "plugins";
    std::error_code ec;
    std::filesystem::create_directories(pluginsDir, ec);
    ASSERT_FALSE(ec);

#if defined(_WIN32)
    const std::string ext = ".dll";
#else
    const std::string ext = ".so";
#endif

    // Two plugin files (deliberately out of order) plus a non-plugin file that must be ignored.
    writeTextFile(pluginsDir / ("bravo" + ext), "b");
    writeTextFile(pluginsDir / ("alpha" + ext), "a");
    writeTextFile(pluginsDir / "notes.txt", "ignore me");

    catalog::PluginLoader loader(
        {.pluginsDir = pluginsDir},
        {}, // real defaultEnumerate
        [](const std::filesystem::path& path) {
            return std::expected<catalog::PluginHandle, std::string>(catalog::PluginHandle::fromExports(
                path, &createOk, &destroyOk, &typeFn, &verFn, "dummy", "1.0.0"));
        });

    const auto results = loader.loadAll();
    ASSERT_EQ(results.size(), 2u);

    const auto canonicalPlugins = std::filesystem::weakly_canonical(pluginsDir, ec);
    ASSERT_FALSE(ec);
    EXPECT_EQ(results[0].path.filename(), std::filesystem::path("alpha" + ext));
    EXPECT_EQ(results[1].path.filename(), std::filesystem::path("bravo" + ext));
    for (const auto& result : results) {
        EXPECT_EQ(result.path.parent_path(), canonicalPlugins);
    }
}

