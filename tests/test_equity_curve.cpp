#include <gtest/gtest.h>

#include "risk/equity_curve.h"
#include "risk/risk_db.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path uniqueDbPath(const std::string& name) {
    const auto tick = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    return std::filesystem::temp_directory_path() / ("bot_equity_" + name + "_" + std::to_string(tick)) / "risk.db";
}

int64_t utcMidnightMs(int year, unsigned month, unsigned day) {
    using namespace std::chrono;
    const auto tp = sys_days{std::chrono::year{year} / month / day};
    return duration_cast<milliseconds>(tp.time_since_epoch()).count();
}

TEST(EquityCurveTest, RecordsSourceBasisAndRejectsNonPositiveEquity) {
    const auto dbPath = uniqueDbPath("record");
    const auto root = dbPath.parent_path();
    {
        engine::RiskDb db(dbPath.string());
        engine::EquityCurve curve(db);

        const int64_t ts = utcMidnightMs(2026, 1, 2);
        curve.recordPeriodic(-1.0, ts, "margin");
        curve.recordTradeClose(0.0, ts, "margin");
        curve.recordPeriodic(100.0, ts, "margin");
        curve.recordTradeClose(101.0, ts + 1000, "wallet");

        const auto points = curve.getByYear(2026);
        ASSERT_EQ(points.size(), 2u);
        EXPECT_EQ(points[0].source, "periodic");
        EXPECT_EQ(points[0].basis, "margin");
        EXPECT_EQ(points[1].source, "trade_close");
        EXPECT_EQ(points[1].basis, "wallet");
    }
    std::filesystem::remove_all(root);
}

TEST(EquityCurveTest, ExtractsUtcYearCorrectly) {
    const auto dbPath = uniqueDbPath("year");
    const auto root = dbPath.parent_path();
    {
        engine::RiskDb db(dbPath.string());
        engine::EquityCurve curve(db);

        const int64_t dec31_2025 = utcMidnightMs(2025, 12, 31);
        const int64_t jan1_2026 = utcMidnightMs(2026, 1, 1);
        curve.recordPeriodic(100.0, dec31_2025, "margin");
        curve.recordPeriodic(101.0, jan1_2026, "margin");

        const auto y2025 = curve.getByYear(2025);
        const auto y2026 = curve.getByYear(2026);
        ASSERT_EQ(y2025.size(), 1u);
        ASSERT_EQ(y2026.size(), 1u);
        EXPECT_EQ(y2025[0].timestampMs, dec31_2025);
        EXPECT_EQ(y2026[0].timestampMs, jan1_2026);
    }
    std::filesystem::remove_all(root);
}

} // namespace
