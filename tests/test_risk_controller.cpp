#include <gtest/gtest.h>

#include "risk/risk_controller.h"

#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#elif __has_include(<winsqlite/winsqlite3.h>)
#include <winsqlite/winsqlite3.h>
#else
#error "SQLite headers not found"
#endif

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

namespace {

std::filesystem::path uniqueDbPath(const std::string& name) {
    const auto tick = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    return std::filesystem::temp_directory_path() / ("bot_risk_ctrl_" + name + "_" + std::to_string(tick)) / "risk.db";
}

account::AccountSnapshot snapshotWithEquity(double margin, double wallet) {
    account::AccountSnapshot s;
    s.account.totalMarginBalance = margin;
    s.account.totalWalletBalance = wallet;
    s.account.availableBalance = margin;
    return s;
}

engine::RiskConfig baseConfig() {
    engine::RiskConfig cfg;
    cfg.enabled = true;
    cfg.equityBasis = engine::RiskEquityBasis::Margin;
    cfg.minDataPoints = 2;
    cfg.sampleIntervalMinutes = 60;
    cfg.controlLookbackDays = 365;
    cfg.metricsComputeIntervalMinutes = 60;
    cfg.softMaxDrawdown = 0.20;
    cfg.hardMaxDrawdown = 0.35;
    cfg.softMinUpi = -10.0;
    cfg.hardMinUpi = -20.0;
    return cfg;
}

void runAwaitable(boost::asio::awaitable<void> task) {
    boost::asio::io_context ioc;
    auto fut = boost::asio::co_spawn(ioc, std::move(task), boost::asio::use_future);
    ioc.run();
    fut.get();
}

void dropMetricsCache(const std::string& dbPath) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(dbPath.c_str(), &db), SQLITE_OK);
    char* err = nullptr;
    const int rc = sqlite3_exec(db, "DROP TABLE risk_metrics_cache;", nullptr, nullptr, &err);
    if (err != nullptr) {
        sqlite3_free(err);
    }
    sqlite3_close(db);
    ASSERT_EQ(rc, SQLITE_OK);
}

TEST(RiskControllerTest, CanOpenPositionRespectsMissingDataMode) {
    auto cfgOpen = baseConfig();
    cfgOpen.missingDataMode = engine::RiskMissingDataMode::Open;
    cfgOpen.dbPath = uniqueDbPath("missing_open").string();
    const auto rootOpen = std::filesystem::path(cfgOpen.dbPath).parent_path();
    {
        engine::RiskDb db(cfgOpen.dbPath);
        engine::EquityCurve curve(db);
        engine::RiskMetrics metrics(0.0, cfgOpen.minDataPoints, std::chrono::minutes{cfgOpen.sampleIntervalMinutes});
        engine::RiskController controller(db, curve, metrics, cfgOpen);
        EXPECT_TRUE(controller.canOpenPosition());
    }
    std::filesystem::remove_all(rootOpen);

    auto cfgClosed = baseConfig();
    cfgClosed.missingDataMode = engine::RiskMissingDataMode::Closed;
    cfgClosed.dbPath = uniqueDbPath("missing_closed").string();
    const auto rootClosed = std::filesystem::path(cfgClosed.dbPath).parent_path();
    {
        engine::RiskDb db(cfgClosed.dbPath);
        engine::EquityCurve curve(db);
        engine::RiskMetrics metrics(0.0, cfgClosed.minDataPoints, std::chrono::minutes{cfgClosed.sampleIntervalMinutes});
        engine::RiskController controller(db, curve, metrics, cfgClosed);
        EXPECT_FALSE(controller.canOpenPosition());
    }
    std::filesystem::remove_all(rootClosed);
}

TEST(RiskControllerTest, HardDrawdownBreachBlocksOpenPosition) {
    auto cfg = baseConfig();
    cfg.dbPath = uniqueDbPath("hard_breach").string();
    const auto root = std::filesystem::path(cfg.dbPath).parent_path();
    {
        engine::RiskDb db(cfg.dbPath);
        engine::EquityCurve curve(db);
        engine::RiskMetrics metrics(0.0, cfg.minDataPoints, std::chrono::minutes{cfg.sampleIntervalMinutes});
        engine::RiskController controller(db, curve, metrics, cfg);

        const int64_t t0 = 1'700'000'000'000;
        const int64_t t1 = t0 + 2LL * 60 * 60 * 1000;
        controller.onScanCycle(snapshotWithEquity(100.0, 100.0), t0);
        controller.onScanCycle(snapshotWithEquity(50.0, 50.0), t1);
        controller.recomputeMetrics(t1);

        EXPECT_EQ(controller.currentStatus(), engine::RiskStatus::HARD_BREACH);
        EXPECT_FALSE(controller.canOpenPosition());
        const auto latest = controller.latestMetrics();
        EXPECT_TRUE(latest.valid);
        EXPECT_LT(latest.maxDrawdown, -0.35);
    }
    std::filesystem::remove_all(root);
}

TEST(RiskControllerTest, WarmStartsHardBreachFromPersistedMetricsCache) {
    auto cfg = baseConfig();
    cfg.missingDataMode = engine::RiskMissingDataMode::Open;
    cfg.dbPath = uniqueDbPath("warm_start").string();
    const auto root = std::filesystem::path(cfg.dbPath).parent_path();
    {
        engine::RiskDb db(cfg.dbPath);
        engine::EquityCurve curve(db);
        engine::RiskMetrics metrics(0.0, cfg.minDataPoints, std::chrono::minutes{cfg.sampleIntervalMinutes});

        const int64_t t0 = 1'700'000'000'000;
        const int64_t t1 = t0 + 2LL * 60 * 60 * 1000;
        {
            engine::RiskController controller(db, curve, metrics, cfg);
            controller.onScanCycle(snapshotWithEquity(100.0, 100.0), t0);
            controller.onScanCycle(snapshotWithEquity(50.0, 50.0), t1);
            controller.recomputeMetrics(t1);
            ASSERT_EQ(controller.currentStatus(), engine::RiskStatus::HARD_BREACH);
        }

        engine::RiskController restarted(db, curve, metrics, cfg);
        EXPECT_EQ(restarted.currentStatus(), engine::RiskStatus::HARD_BREACH);
        EXPECT_FALSE(restarted.canOpenPosition());
        EXPECT_TRUE(restarted.latestMetrics().valid);
    }
    std::filesystem::remove_all(root);
}

TEST(RiskControllerTest, MaybeRecomputeHonorsIntervalGate) {
    auto cfg = baseConfig();
    cfg.metricsComputeIntervalMinutes = 60;
    cfg.dbPath = uniqueDbPath("interval").string();
    const auto root = std::filesystem::path(cfg.dbPath).parent_path();
    {
        engine::RiskDb db(cfg.dbPath);
        engine::EquityCurve curve(db);
        engine::RiskMetrics metrics(0.0, cfg.minDataPoints, std::chrono::minutes{cfg.sampleIntervalMinutes});
        engine::RiskController controller(db, curve, metrics, cfg);

        const int64_t t0 = 1'700'000'000'000;
        const int64_t t1 = t0 + 2LL * 60 * 60 * 1000;
        controller.onScanCycle(snapshotWithEquity(100.0, 100.0), t0);
        controller.onScanCycle(snapshotWithEquity(101.0, 101.0), t1);
        runAwaitable(controller.maybeRecompute(t1));
        const auto first = controller.latestMetrics();
        ASSERT_EQ(first.computedAtMs, t1);

        const int64_t t2 = t1 + 10LL * 60 * 1000;
        controller.onScanCycle(snapshotWithEquity(102.0, 102.0), t2);
        runAwaitable(controller.maybeRecompute(t2));
        const auto second = controller.latestMetrics();
        EXPECT_EQ(second.computedAtMs, first.computedAtMs);
        EXPECT_EQ(second.dataPoints, first.dataPoints);
    }
    std::filesystem::remove_all(root);
}

TEST(RiskControllerTest, RollingWindowExcludesOldPoints) {
    auto cfg = baseConfig();
    cfg.controlLookbackDays = 1;
    cfg.missingDataMode = engine::RiskMissingDataMode::Closed;
    cfg.dbPath = uniqueDbPath("window").string();
    const auto root = std::filesystem::path(cfg.dbPath).parent_path();
    {
        engine::RiskDb db(cfg.dbPath);
        engine::EquityCurve curve(db);
        engine::RiskMetrics metrics(0.0, cfg.minDataPoints, std::chrono::minutes{cfg.sampleIntervalMinutes});
        engine::RiskController controller(db, curve, metrics, cfg);

        const int64_t now = 1'700'000'000'000;
        const int64_t threeDaysMs = 3LL * 24 * 60 * 60 * 1000;
        const int64_t oneHourMs = 60LL * 60 * 1000;
        controller.onScanCycle(snapshotWithEquity(100.0, 100.0), now - threeDaysMs);
        controller.onScanCycle(snapshotWithEquity(110.0, 110.0), now - oneHourMs);
        controller.recomputeMetrics(now);

        const auto latest = controller.latestMetrics();
        EXPECT_EQ(latest.dataPoints, 1);
        EXPECT_FALSE(latest.valid);
        EXPECT_FALSE(controller.canOpenPosition());
    }
    std::filesystem::remove_all(root);
}

TEST(RiskControllerTest, FailureModeClosedFailsClosedAndOpenLeavesGateOpen) {
    auto cfgClosed = baseConfig();
    cfgClosed.failureMode = engine::RiskFailureMode::Closed;
    cfgClosed.missingDataMode = engine::RiskMissingDataMode::Open;
    cfgClosed.dbPath = uniqueDbPath("failure_closed").string();
    const auto rootClosed = std::filesystem::path(cfgClosed.dbPath).parent_path();
    {
        engine::RiskDb db(cfgClosed.dbPath);
        engine::EquityCurve curve(db);
        engine::RiskMetrics metrics(0.0, cfgClosed.minDataPoints, std::chrono::minutes{cfgClosed.sampleIntervalMinutes});
        engine::RiskController controller(db, curve, metrics, cfgClosed);
        controller.onScanCycle(snapshotWithEquity(100.0, 100.0), 1'700'000'000'000);
        controller.onScanCycle(snapshotWithEquity(101.0, 101.0), 1'700'000'000'000 + 2LL * 60 * 60 * 1000);
        dropMetricsCache(cfgClosed.dbPath);

        runAwaitable(controller.maybeRecompute(1'700'000'000'000 + 2LL * 60 * 60 * 1000));
        EXPECT_EQ(controller.currentStatus(), engine::RiskStatus::HARD_BREACH);
        EXPECT_FALSE(controller.canOpenPosition());
    }
    std::filesystem::remove_all(rootClosed);

    auto cfgOpen = cfgClosed;
    cfgOpen.failureMode = engine::RiskFailureMode::Open;
    cfgOpen.dbPath = uniqueDbPath("failure_open").string();
    const auto rootOpen = std::filesystem::path(cfgOpen.dbPath).parent_path();
    {
        engine::RiskDb db(cfgOpen.dbPath);
        engine::EquityCurve curve(db);
        engine::RiskMetrics metrics(0.0, cfgOpen.minDataPoints, std::chrono::minutes{cfgOpen.sampleIntervalMinutes});
        engine::RiskController controller(db, curve, metrics, cfgOpen);
        controller.onScanCycle(snapshotWithEquity(100.0, 100.0), 1'700'000'000'000);
        controller.onScanCycle(snapshotWithEquity(101.0, 101.0), 1'700'000'000'000 + 2LL * 60 * 60 * 1000);
        dropMetricsCache(cfgOpen.dbPath);

        runAwaitable(controller.maybeRecompute(1'700'000'000'000 + 2LL * 60 * 60 * 1000));
        EXPECT_EQ(controller.currentStatus(), engine::RiskStatus::OK);
        EXPECT_TRUE(controller.canOpenPosition());
    }
    std::filesystem::remove_all(rootOpen);
}

TEST(RiskControllerTest, EquityBasisSelectsWalletBalance) {
    auto cfg = baseConfig();
    cfg.equityBasis = engine::RiskEquityBasis::Wallet;
    cfg.dbPath = uniqueDbPath("wallet_basis").string();
    const auto root = std::filesystem::path(cfg.dbPath).parent_path();
    {
        engine::RiskDb db(cfg.dbPath);
        engine::EquityCurve curve(db);
        engine::RiskMetrics metrics(0.0, cfg.minDataPoints, std::chrono::minutes{cfg.sampleIntervalMinutes});
        engine::RiskController controller(db, curve, metrics, cfg);

        const int64_t t0 = 1'700'000'000'000;
        const int64_t t1 = t0 + 2LL * 60 * 60 * 1000;
        controller.onScanCycle(snapshotWithEquity(100.0, 500.0), t0);
        controller.onScanCycle(snapshotWithEquity(50.0, 510.0), t1);
        controller.recomputeMetrics(t1);

        EXPECT_EQ(controller.currentStatus(), engine::RiskStatus::OK);
        const auto latest = controller.latestMetrics();
        EXPECT_EQ(latest.basis, "wallet");
        EXPECT_GT(latest.annualReturn, 0.0);
    }
    std::filesystem::remove_all(root);
}

TEST(RiskControllerTest, SoftDrawdownAndUpiBreachesSetSoftStatus) {
    auto cfg = baseConfig();
    cfg.softMaxDrawdown = 0.05;
    cfg.hardMaxDrawdown = 0.50;
    cfg.dbPath = uniqueDbPath("soft_drawdown").string();
    const auto root = std::filesystem::path(cfg.dbPath).parent_path();
    {
        engine::RiskDb db(cfg.dbPath);
        engine::EquityCurve curve(db);
        engine::RiskMetrics metrics(0.0, cfg.minDataPoints, std::chrono::minutes{cfg.sampleIntervalMinutes});
        engine::RiskController controller(db, curve, metrics, cfg);

        const int64_t t0 = 1'700'000'000'000;
        const int64_t t1 = t0 + 2LL * 60 * 60 * 1000;
        controller.onScanCycle(snapshotWithEquity(100.0, 100.0), t0);
        controller.onScanCycle(snapshotWithEquity(90.0, 90.0), t1);
        controller.recomputeMetrics(t1);

        EXPECT_EQ(controller.currentStatus(), engine::RiskStatus::SOFT_BREACH);
        EXPECT_TRUE(controller.canOpenPosition());
    }
    std::filesystem::remove_all(root);

    auto upiCfg = baseConfig();
    upiCfg.softMaxDrawdown = 0.20;
    upiCfg.hardMaxDrawdown = 0.50;
    upiCfg.softMinUpi = 1.0;
    upiCfg.hardMinUpi = -1.0;
    upiCfg.dbPath = uniqueDbPath("soft_upi").string();
    const auto upiRoot = std::filesystem::path(upiCfg.dbPath).parent_path();
    {
        engine::RiskDb db(upiCfg.dbPath);
        engine::EquityCurve curve(db);
        engine::RiskMetrics metrics(0.0, upiCfg.minDataPoints, std::chrono::minutes{upiCfg.sampleIntervalMinutes});
        engine::RiskController controller(db, curve, metrics, upiCfg);

        const int64_t t0 = 1'700'000'000'000;
        const int64_t t1 = t0 + 1LL * 60 * 60 * 1000;
        const int64_t t2 = t0 + 2LL * 60 * 60 * 1000;
        controller.onScanCycle(snapshotWithEquity(100.0, 100.0), t0);
        controller.onScanCycle(snapshotWithEquity(99.0, 99.0), t1);
        controller.onScanCycle(snapshotWithEquity(100.0, 100.0), t2);
        controller.recomputeMetrics(t2);

        EXPECT_EQ(controller.currentStatus(), engine::RiskStatus::SOFT_BREACH);
        EXPECT_TRUE(controller.latestMetrics().valid);
        EXPECT_LT(controller.latestMetrics().upi, upiCfg.softMinUpi);
    }
    std::filesystem::remove_all(upiRoot);
}

TEST(RiskControllerTest, RiskConfigRejectsInvalidEnumValues) {
    auto j = nlohmann::json::object();
    j["enabled"] = true;

    j["equity_basis"] = "invalid";
    EXPECT_THROW((void)engine::RiskConfig::fromJson(j), std::invalid_argument);

    j["equity_basis"] = "margin";
    j["missing_data_mode"] = "invalid";
    EXPECT_THROW((void)engine::RiskConfig::fromJson(j), std::invalid_argument);

    j["missing_data_mode"] = "open";
    j["failure_mode"] = "invalid";
    EXPECT_THROW((void)engine::RiskConfig::fromJson(j), std::invalid_argument);
}

TEST(RiskControllerTest, RiskConfigRejectsInvertedUpiThresholds) {
    auto j = nlohmann::json::object();
    j["hard_min_upi"] = 1.0;
    j["soft_min_upi"] = 0.5;
    EXPECT_THROW((void)engine::RiskConfig::fromJson(j), std::invalid_argument);
}

} // namespace
