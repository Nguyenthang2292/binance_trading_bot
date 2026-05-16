#include <gtest/gtest.h>

#include "catalog/plugin_loader.h"

#include <chrono>
#include <fstream>

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

