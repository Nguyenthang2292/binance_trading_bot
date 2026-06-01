#include <gtest/gtest.h>

#include "orchestration/process_manager.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

struct TempDirGuard {
    std::filesystem::path path;
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::filesystem::path makeTempDir() {
    const auto base = std::filesystem::temp_directory_path();
    const auto suffix = std::to_string(
        static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = base / ("process_manager_test_" + suffix);
    std::filesystem::create_directories(path);
    return path;
}

std::vector<std::string> cmdExit0() {
#if defined(_WIN32)
    return {"cmd.exe", "/c", "exit 0"};
#else
    return {"/bin/sh", "-c", "exit 0"};
#endif
}

std::vector<std::string> cmdExit1() {
#if defined(_WIN32)
    return {"cmd.exe", "/c", "exit 1"};
#else
    return {"/bin/sh", "-c", "exit 1"};
#endif
}

std::vector<std::string> cmdLongRunning() {
#if defined(_WIN32)
    return {"cmd.exe", "/c", "ping 127.0.0.1 -n 30 >nul"};
#else
    return {"/bin/sh", "-c", "sleep 30"};
#endif
}

std::vector<std::string> cmdPrintSecretEnv() {
#if defined(_WIN32)
    return {"cmd.exe", "/c", "if defined BINANCE_API_SECRET (echo leaked) else (echo missing)"};
#else
    return {"/bin/sh", "-c", "if [ -n \"$BINANCE_API_SECRET\" ]; then echo leaked; else echo missing; fi"};
#endif
}

std::vector<std::string> cmdEchoSecretLikeOutput() {
#if defined(_WIN32)
    return {"cmd.exe", "/c", "echo BINANCE_API_SECRET=supersecret"};
#else
    return {"/bin/sh", "-c", "echo BINANCE_API_SECRET=supersecret"};
#endif
}

void setEnvVar(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void unsetEnvVar(const char* name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

class EnvVarGuard {
public:
    EnvVarGuard(const char* name, const std::string& value)
        : m_name(name) {
        if (const char* previous = std::getenv(name)) {
            m_hadPrevious = true;
            m_previous = previous;
        }
        setEnvVar(name, value);
    }

    ~EnvVarGuard() {
        if (m_hadPrevious) {
            setEnvVar(m_name.c_str(), m_previous);
        } else {
            unsetEnvVar(m_name.c_str());
        }
    }

private:
    std::string m_name;
    bool m_hadPrevious{false};
    std::string m_previous;
};

std::string readFile(const std::string& path) {
    std::ifstream input(path);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

} // namespace

TEST(ProcessManagerTest, SpawnSuccessExitsZero) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};

    orchestration::ProcessManager pm(orchestration::ProcessManagerConfig{
        .maxAttempts = 1,
        .backoffBaseSeconds = 1,
        .timeoutSeconds = 10,
        .logDir = tmp.string(),
    });

    const auto result = pm.spawnWithRetry(cmdExit0());
    EXPECT_TRUE(result.succeeded);
    EXPECT_FALSE(result.timedOut);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_TRUE(std::filesystem::exists(result.logPath));
}

TEST(ProcessManagerTest, SpawnFailureExitsNonZero) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};

    orchestration::ProcessManager pm(orchestration::ProcessManagerConfig{
        .maxAttempts = 3,
        .backoffBaseSeconds = 0,
        .timeoutSeconds = 10,
        .logDir = tmp.string(),
    });

    const auto result = pm.spawnWithRetry(cmdExit1());
    EXPECT_FALSE(result.succeeded);
    EXPECT_FALSE(result.timedOut);
    EXPECT_NE(result.exitCode, 0);
}

TEST(ProcessManagerTest, RetryBackoffRespected) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};

    orchestration::ProcessManager pm(orchestration::ProcessManagerConfig{
        .maxAttempts = 2,
        .backoffBaseSeconds = 1,
        .timeoutSeconds = 10,
        .logDir = tmp.string(),
    });

    const auto start = std::chrono::steady_clock::now();
    (void)pm.spawnWithRetry(cmdExit1());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 900);
}

TEST(ProcessManagerTest, TimeoutKillsProcess) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};

    orchestration::ProcessManager pm(orchestration::ProcessManagerConfig{
        .maxAttempts = 1,
        .backoffBaseSeconds = 1,
        .timeoutSeconds = 2,
        .logDir = tmp.string(),
    });

    const auto result = pm.spawnWithRetry(cmdLongRunning());
    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(result.timedOut);
}

TEST(ProcessManagerTest, LogFileCreated) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};

    orchestration::ProcessManager pm(orchestration::ProcessManagerConfig{
        .maxAttempts = 1,
        .backoffBaseSeconds = 0,
        .timeoutSeconds = 10,
        .logDir = tmp.string(),
    });

    const auto result = pm.spawnWithRetry(cmdExit0());
    EXPECT_FALSE(result.logPath.empty());
    EXPECT_TRUE(std::filesystem::exists(result.logPath));
}

TEST(ProcessManagerTest, SpawnUsesSanitizedEnvironment) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};
    EnvVarGuard secret("BINANCE_API_SECRET", "do-not-inherit");

    orchestration::ProcessManager pm(orchestration::ProcessManagerConfig{
        .maxAttempts = 1,
        .backoffBaseSeconds = 0,
        .timeoutSeconds = 10,
        .logDir = tmp.string(),
    });

    const auto result = pm.spawnWithRetry(cmdPrintSecretEnv());
    EXPECT_TRUE(result.succeeded);
    EXPECT_NE(readFile(result.logPath).find("missing"), std::string::npos);
    EXPECT_EQ(readFile(result.logPath).find("leaked"), std::string::npos);
}

TEST(ProcessManagerTest, SpawnRedactsSecretLikeOutput) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};

    orchestration::ProcessManager pm(orchestration::ProcessManagerConfig{
        .maxAttempts = 1,
        .backoffBaseSeconds = 0,
        .timeoutSeconds = 10,
        .logDir = tmp.string(),
    });

    const auto result = pm.spawnWithRetry(cmdEchoSecretLikeOutput());
    EXPECT_TRUE(result.succeeded);
    const std::string logText = readFile(result.logPath);
    EXPECT_EQ(logText.find("supersecret"), std::string::npos);
    EXPECT_NE(logText.find("[REDACTED]"), std::string::npos);
}

#if !defined(_WIN32)
TEST(ProcessManagerTest, PosixExecFailureWritesDiagnosticToLog) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};

    orchestration::ProcessManager pm(orchestration::ProcessManagerConfig{
        .maxAttempts = 1,
        .backoffBaseSeconds = 0,
        .timeoutSeconds = 10,
        .logDir = tmp.string(),
    });

    const auto result = pm.spawnWithRetry({"/definitely/not/a/real/executable"});
    EXPECT_FALSE(result.succeeded);
    EXPECT_FALSE(result.timedOut);
    EXPECT_EQ(result.exitCode, 127);

    std::istringstream contents(readFile(result.logPath));
    const auto logText = contents.str();
    EXPECT_NE(logText.find("execvp failed errno="), std::string::npos);
    EXPECT_NE(logText.find("cmd=/definitely/not/a/real/executable"), std::string::npos);
}
#endif
