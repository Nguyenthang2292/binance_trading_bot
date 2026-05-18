#include <gtest/gtest.h>

#include "risk/risk_controller.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <string>

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
