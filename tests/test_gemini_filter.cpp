#include <gtest/gtest.h>

#include "engine/gemini_filter.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

struct TempDirGuard {
    fs::path path;
    ~TempDirGuard() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

fs::path makeTempDir() {
    const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::mt19937_64 rng(static_cast<std::mt19937_64::result_type>(nowNs));
    std::uniform_int_distribution<unsigned long long> dist;
    const auto leaf = "gemini_filter_it_" + std::to_string(nowNs) + "_" + std::to_string(dist(rng));
    const fs::path out = fs::temp_directory_path() / leaf;
    fs::create_directories(out);
    return out;
}

void writeTextFile(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good()) << "failed to open " << path.string();
    out << text;
    out.flush();
    ASSERT_TRUE(out.good()) << "failed to write " << path.string();
}

void writeFakeEvalModule(const fs::path& workingDir) {
    constexpr std::string_view module = R"(import json
payload = {
  "eval_id": "test",
  "decision": "Allow",
  "confidence": 0.9,
  "sentiment_score": 0.9,
  "vision_score": 0.9,
  "sentiment_analysis": "ok",
  "vision_analysis": "ok",
  "reason": "test pass",
  "error_code": None,
  "error": None,
  "latency_ms": 1
}
print(json.dumps(payload, ensure_ascii=True), end="")
)";
    writeTextFile(workingDir / "fake_eval.py", module);
}

void writeAutotuneConfig(const fs::path& configPath, const fs::path& runtimeBase, std::string_view mode) {
    json cfg = {
        {"gemini_filter",
         {
             {"runtime_dir", runtimeBase.string()},
             {"autotune",
              {
                  {"enabled", true},
                  {"mode", mode},
                  {"interval_seconds", 1},
                  {"controller_timeout_seconds", 1},
              }},
         }},
    };
    writeTextFile(configPath, cfg.dump());
}

void seedCache(scanner::KlineCache& cache) {
    Kline k;
    k.openTime = 1;
    k.closeTime = 2;
    k.open = 100.0;
    k.high = 101.0;
    k.low = 99.0;
    k.close = 100.0;
    k.volume = 1.0;
    cache.update("BTCUSDT", "1h", k);
    cache.update("BTCUSDT", "4h", k);
}

engine::GeminiFilterConfig makeBaseConfig(const fs::path& workingDir, const fs::path& runtimeBase) {
    engine::GeminiFilterConfig cfg;
    cfg.enabled = true;
    cfg.mode = engine::GeminiFilterMode::Enforce;
    cfg.pythonPath = "python";
    cfg.moduleName = "fake_eval";
    cfg.workingDirectory = workingDir.string();
    cfg.runtimeDir = runtimeBase.string();
    cfg.timeoutSeconds = 5;
    cfg.autotuneEnabled = true;
    cfg.autotuneMode = "apply";
    cfg.autotuneIntervalSeconds = 1;
    cfg.autotuneControllerTimeoutSeconds = 1;
    cfg.autotuneConfigPath = "config.json";
    return cfg;
}

void writeSleepingAutotuneModule(const fs::path& workingDir, int sleepSeconds) {
    writeTextFile(workingDir / "tools" / "__init__.py", "");
    writeTextFile(workingDir / "tools" / "gemini_filter" / "__init__.py", "");
    const std::string module =
        "import argparse\n"
        "from pathlib import Path\n"
        "import time\n"
        "p=argparse.ArgumentParser()\n"
        "p.add_argument('--config')\n"
        "p.add_argument('--runtime-base-dir')\n"
        "a=p.parse_args()\n"
        "rb=Path(a.runtime_base_dir)\n"
        "d=rb/'cache'/'autotune'\n"
        "d.mkdir(parents=True, exist_ok=True)\n"
        "(d/'invoked.txt').write_text('invoked', encoding='utf-8')\n"
        "lock=d/'controller.lock'\n"
        "if lock.exists():\n"
        "  (d/'skipped_lock_busy.txt').write_text('1', encoding='utf-8')\n"
        "  raise SystemExit(0)\n"
        "lock.write_text('locked', encoding='utf-8')\n"
        "(d/'started.txt').write_text('started', encoding='utf-8')\n"
        "time.sleep(" + std::to_string(sleepSeconds) + ")\n"
        "(d/'completed.txt').write_text('completed', encoding='utf-8')\n";
    const std::string moduleWithUnlock =
        module +
        "lock.unlink(missing_ok=True)\n";
    writeTextFile(workingDir / "tools" / "gemini_filter" / "autotune.py", moduleWithUnlock);
}

} // namespace

TEST(GeminiFilterControllerIntegrationTest, AutotuneLockContentionSkipsControllerWork) {
    TempDirGuard temp{makeTempDir()};
    const fs::path workingDir = temp.path / "work";
    const fs::path runtimeBase = temp.path / "runtime";
    fs::create_directories(workingDir);
    fs::create_directories(runtimeBase / "cache" / "autotune");
    writeFakeEvalModule(workingDir);
    writeSleepingAutotuneModule(workingDir, 1);
    writeAutotuneConfig(workingDir / "config.json", runtimeBase, "apply");
    writeTextFile(runtimeBase / "cache" / "autotune" / "controller.lock", "busy");

    auto cfg = makeBaseConfig(workingDir, runtimeBase);
    engine::GeminiFilterController controller(cfg);
    scanner::KlineCache cache(200);
    seedCache(cache);

    const auto result = controller.evaluate("BTCUSDT", strategy::Signal::Direction::Long, "1h", cache);
    EXPECT_EQ(result.decision, engine::GeminiDecision::Allow);
    EXPECT_FALSE(result.hasError);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_TRUE(fs::exists(runtimeBase / "cache" / "autotune" / "invoked.txt"));
    EXPECT_TRUE(fs::exists(runtimeBase / "cache" / "autotune" / "controller.lock"));
    EXPECT_TRUE(fs::exists(runtimeBase / "cache" / "autotune" / "skipped_lock_busy.txt"));
    EXPECT_FALSE(fs::exists(runtimeBase / "cache" / "autotune" / "started.txt"));
    EXPECT_FALSE(fs::exists(runtimeBase / "cache" / "autotune" / "completed.txt"));
    EXPECT_FALSE(fs::exists(runtimeBase / "cache" / "autotune" / "last_recommendation.json"));
    EXPECT_FALSE(fs::exists(runtimeBase / "cache" / "autotune" / "active_override.json"));
}

TEST(GeminiFilterControllerIntegrationTest, AutotuneTimeoutDoesNotBlockEvaluateFlow) {
    TempDirGuard temp{makeTempDir()};
    const fs::path workingDir = temp.path / "work";
    const fs::path runtimeBase = temp.path / "runtime";
    fs::create_directories(workingDir);
    fs::create_directories(runtimeBase);
    writeFakeEvalModule(workingDir);
    writeAutotuneConfig(workingDir / "config.json", runtimeBase, "apply");
    writeSleepingAutotuneModule(workingDir, 11);

    auto cfg = makeBaseConfig(workingDir, runtimeBase);
    cfg.autotuneControllerTimeoutSeconds = 1;
    cfg.timeoutSeconds = 5;
    engine::GeminiFilterController controller(cfg);
    scanner::KlineCache cache(200);
    seedCache(cache);

    const auto started = std::chrono::steady_clock::now();
    const auto result = controller.evaluate("BTCUSDT", strategy::Signal::Direction::Long, "1h", cache);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    EXPECT_EQ(result.decision, engine::GeminiDecision::Allow);
    EXPECT_FALSE(result.hasError);
    EXPECT_LT(elapsedMs, 2500) << "evaluate path should not wait for autotune completion";

    std::this_thread::sleep_for(std::chrono::milliseconds(12500));

    EXPECT_TRUE(fs::exists(runtimeBase / "cache" / "autotune" / "invoked.txt"));
    EXPECT_TRUE(fs::exists(runtimeBase / "cache" / "autotune" / "started.txt"));
    EXPECT_FALSE(fs::exists(runtimeBase / "cache" / "autotune" / "completed.txt"));
    EXPECT_TRUE(fs::exists(runtimeBase / "cache" / "autotune" / "controller.lock"));
}
