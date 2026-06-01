#include <gtest/gtest.h>

#include "orchestration/batch_scheduler_thread.h"

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
    const auto path = base / ("batch_scheduler_test_" + suffix);
    std::filesystem::create_directories(path);
    return path;
}

class FakeProcessRunner final : public orchestration::IProcessRunner {
public:
    explicit FakeProcessRunner(std::vector<orchestration::ProcessResult> scripted)
        : m_scripted(std::move(scripted)) {}

    orchestration::ProcessResult spawnWithRetry(const std::vector<std::string>& cmd) override {
        calls.push_back(cmd);
        if (nextIndex >= m_scripted.size()) {
            return orchestration::ProcessResult{};
        }
        return m_scripted[nextIndex++];
    }

    bool startDaemon(const std::string& name, const std::vector<std::string>& cmd) override { return true; }
    bool isDaemonRunning(const std::string& name) override { return false; }
    void stopDaemon(const std::string& name) override {}

    std::vector<std::vector<std::string>> calls;
    size_t nextIndex{0};

private:
    std::vector<orchestration::ProcessResult> m_scripted;
};

std::chrono::system_clock::time_point makeUtcTimePoint(
    int year,
    int month,
    int day,
    int hour,
    int minute) {
    std::tm tmLocal{};
    tmLocal.tm_year = year - 1900;
    tmLocal.tm_mon = month - 1;
    tmLocal.tm_mday = day;
    tmLocal.tm_hour = hour;
    tmLocal.tm_min = minute;
    tmLocal.tm_sec = 0;
    tmLocal.tm_isdst = 0;
#if defined(_WIN32)
    const std::time_t t = _mkgmtime(&tmLocal);
#else
    const std::time_t t = timegm(&tmLocal);
#endif
    return std::chrono::system_clock::from_time_t(t);
}

orchestration::BatchSchedulerConfig makeConfig() {
    orchestration::BatchSchedulerConfig cfg;
    cfg.pythonExe = "python";
    cfg.scriptsDir = "tools/qlib_bridge";
    cfg.dataDir = "data/qlib";
    cfg.modelDir = "data/qlib";
    cfg.dbPath = "data/qlib_predictions.db";
    cfg.modelId = "lightgbm_1h_v1";
    cfg.interval = "1h";
    cfg.symbols = {"BTCUSDT", "ETHUSDT"};
    cfg.batchHour = 7;
    cfg.batchMinute = 0;
    cfg.batchWeekdays = {0, 1, 2, 3, 4};
    return cfg;
}

bool commandHasToken(const std::vector<std::string>& cmd, std::string_view token) {
    for (const auto& part : cmd) {
        if (part.find(token) != std::string::npos) {
            return true;
        }
    }
    return false;
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

} // namespace

TEST(BatchSchedulerThreadTest, DoesNotRunOnWeekend) {
    FakeProcessRunner runner({});
    orchestration::BatchSchedulerThread scheduler(makeConfig(), runner);

    const auto saturday0700 = makeUtcTimePoint(2026, 5, 23, 7, 0);
    EXPECT_FALSE(scheduler.shouldRunToday(saturday0700));
}

TEST(BatchSchedulerThreadTest, DoesNotRunBeforeScheduledHour) {
    FakeProcessRunner runner({});
    orchestration::BatchSchedulerThread scheduler(makeConfig(), runner);

    const auto monday0659 = makeUtcTimePoint(2026, 5, 18, 6, 59);
    const auto monday0700 = makeUtcTimePoint(2026, 5, 18, 7, 0);
    EXPECT_FALSE(scheduler.shouldRunToday(monday0659));
    EXPECT_TRUE(scheduler.shouldRunToday(monday0700));
}

TEST(BatchSchedulerThreadTest, FailedRunDoesNotSuppressSameDayRetry) {
    FakeProcessRunner runner({
        orchestration::ProcessResult{.exitCode = 1, .timedOut = false, .succeeded = false, .logPath = "p1.log"},
        orchestration::ProcessResult{.exitCode = 1, .timedOut = false, .succeeded = false, .logPath = "p1-retry.log"},
    });
    orchestration::BatchSchedulerThread scheduler(makeConfig(), runner);

    const auto monday0700 = makeUtcTimePoint(2026, 5, 18, 7, 0);
    EXPECT_TRUE(scheduler.runScheduledCycleAt(monday0700));
    EXPECT_TRUE(scheduler.runScheduledCycleAt(monday0700));
    ASSERT_EQ(runner.calls.size(), 2U);
}

TEST(BatchSchedulerThreadTest, Phase2SkippedWhenPhase1Fails) {
    FakeProcessRunner runner({
        orchestration::ProcessResult{.exitCode = 1, .timedOut = false, .succeeded = false, .logPath = "p1.log"},
    });
    orchestration::BatchSchedulerThread scheduler(makeConfig(), runner);

    EXPECT_FALSE(scheduler.runOnce());
    ASSERT_EQ(runner.calls.size(), 1U);
    EXPECT_TRUE(commandHasToken(runner.calls.front(), "export_binance_klines.py"));
}

TEST(BatchSchedulerThreadTest, Phase2RunsAfterPhase1Success) {
    FakeProcessRunner runner({
        orchestration::ProcessResult{.exitCode = 0, .timedOut = false, .succeeded = true, .logPath = "p1.log"},
        orchestration::ProcessResult{.exitCode = 1, .timedOut = false, .succeeded = false, .logPath = "p2.log"},
    });
    orchestration::BatchSchedulerThread scheduler(makeConfig(), runner);

    EXPECT_FALSE(scheduler.runOnce());
    ASSERT_EQ(runner.calls.size(), 2U);
    EXPECT_TRUE(commandHasToken(runner.calls[0], "export_binance_klines.py"));
    EXPECT_TRUE(commandHasToken(runner.calls[1], "train_workflow.py"));
}

TEST(BatchSchedulerThreadTest, Phase1AcceptsDumpBinPathUnderScriptsDir) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};
    const auto scriptsDir = tmp / "tools" / "qlib_bridge";
    const auto dumpScript = scriptsDir / "dump_bin.py";
    std::filesystem::create_directories(scriptsDir);
    std::ofstream(dumpScript) << "print('ok')\n";
    EnvVarGuard env("QLIB_DUMP_BIN_PATH", dumpScript.string());

    auto cfg = makeConfig();
    cfg.scriptsDir = scriptsDir.string();
    FakeProcessRunner runner({});
    orchestration::BatchSchedulerThread scheduler(cfg, runner);

    const auto cmd = scheduler.buildPhase1Cmd();
    EXPECT_TRUE(commandHasToken(cmd, "--dump-bin-script"));
    EXPECT_TRUE(commandHasToken(cmd, "dump_bin.py"));
    EXPECT_TRUE(commandHasToken(cmd, "incremental"));
}

TEST(BatchSchedulerThreadTest, Phase1RejectsDumpBinPathOutsideAllowedRoots) {
    const auto tmp = makeTempDir();
    TempDirGuard guard{tmp};
    const auto scriptsDir = tmp / "tools" / "qlib_bridge";
    const auto outsideDir = tmp / "outside";
    const auto dumpScript = outsideDir / "dump_bin.py";
    std::filesystem::create_directories(scriptsDir);
    std::filesystem::create_directories(outsideDir);
    std::ofstream(dumpScript) << "print('bad')\n";
    EnvVarGuard env("QLIB_DUMP_BIN_PATH", dumpScript.string());

    auto cfg = makeConfig();
    cfg.scriptsDir = scriptsDir.string();
    FakeProcessRunner runner({});
    orchestration::BatchSchedulerThread scheduler(cfg, runner);

    const auto cmd = scheduler.buildPhase1Cmd();
    EXPECT_FALSE(commandHasToken(cmd, "--dump-bin-script"));
    EXPECT_TRUE(commandHasToken(cmd, "none"));
}
