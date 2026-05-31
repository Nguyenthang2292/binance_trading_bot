#include <gtest/gtest.h>

#include "risk/risk_db.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>

namespace {

std::filesystem::path uniqueDbPath(const std::string& name) {
    const auto tick = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    return std::filesystem::temp_directory_path() / ("bot_risk_" + name + "_" + std::to_string(tick)) / "risk.db";
}

TEST(RiskDbTest, CreatesParentDirectoryAndSupportsEquityQueries) {
    const auto dbPath = uniqueDbPath("db");
    const auto parent = dbPath.parent_path();
    ASSERT_FALSE(std::filesystem::exists(parent));

    {
        engine::RiskDb db(dbPath.string());
        EXPECT_TRUE(std::filesystem::exists(parent));

        db.insertEquityPoint(engine::EquityPoint{
            .timestampMs = 1000,
            .equity = 100.0,
            .year = 2026,
            .source = "periodic",
            .basis = "margin",
        });
        db.insertEquityPoint(engine::EquityPoint{
            .timestampMs = 2000,
            .equity = 101.0,
            .year = 2026,
            .source = "trade_close",
            .basis = "wallet",
        });
        db.insertEquityPoint(engine::EquityPoint{
            .timestampMs = 3000,
            .equity = 102.0,
            .year = 2026,
            .source = "periodic",
            .basis = "margin",
        });

        const auto byYear = db.getByYear(2026);
        ASSERT_EQ(byYear.size(), 3u);
        EXPECT_EQ(byYear[0].timestampMs, 1000);
        EXPECT_EQ(byYear[1].timestampMs, 2000);
        EXPECT_EQ(byYear[2].timestampMs, 3000);

        const auto marginRange = db.getByTimeRange("margin", 0, 2500);
        ASSERT_EQ(marginRange.size(), 1u);
        EXPECT_EQ(marginRange[0].timestampMs, 1000);
        EXPECT_EQ(marginRange[0].basis, "margin");
    }

    std::filesystem::remove_all(parent);
}

TEST(RiskDbTest, ReturnsLatestMetricsByWindowAndBasis) {
    const auto dbPath = uniqueDbPath("metrics");
    const auto root = dbPath.parent_path();
    {
        engine::RiskDb db(dbPath.string());

        engine::RiskMetricsResult first;
        first.windowKind = "rolling";
        first.windowStartMs = 1000;
        first.windowEndMs = 2000;
        first.computedAtMs = 2100;
        first.basis = "margin";
        first.dataPoints = 10;
        first.valid = true;
        first.upi = 0.5;
        db.insertMetrics(first);

        engine::RiskMetricsResult second = first;
        second.windowStartMs = 2000;
        second.windowEndMs = 3000;
        second.computedAtMs = 3100;
        second.upi = 0.8;
        db.insertMetrics(second);

        const auto latest = db.getLatestMetrics("rolling", "margin");
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->windowEndMs, 3000);
        EXPECT_DOUBLE_EQ(latest->upi, 0.8);

        const auto none = db.getLatestMetrics("calendar_year", "margin");
        EXPECT_FALSE(none.has_value());
    }

    std::filesystem::remove_all(root);
}

TEST(RiskDbTest, PersistsNonFiniteMetricsAsNullAndReadsSafeFiniteDefaults) {
    const auto dbPath = uniqueDbPath("metrics_inf");
    const auto root = dbPath.parent_path();
    {
        engine::RiskDb db(dbPath.string());

        engine::RiskMetricsResult metrics;
        metrics.windowKind = "rolling";
        metrics.windowStartMs = 1000;
        metrics.windowEndMs = 2000;
        metrics.computedAtMs = 2100;
        metrics.basis = "margin";
        metrics.dataPoints = 10;
        metrics.valid = true;
        metrics.sortinoRatio = std::numeric_limits<double>::infinity();
        metrics.sharpeRatio = std::numeric_limits<double>::quiet_NaN();
        metrics.upi = std::numeric_limits<double>::infinity();
        db.insertMetrics(metrics);

        const auto latest = db.getLatestMetrics("rolling", "margin");
        ASSERT_TRUE(latest.has_value());
        EXPECT_DOUBLE_EQ(latest->sharpeRatio, 0.0);
        EXPECT_DOUBLE_EQ(latest->sortinoRatio, 0.0);
        EXPECT_DOUBLE_EQ(latest->upi, 0.0);
    }

    std::filesystem::remove_all(root);
}

TEST(RiskDbTest, DeletesEquityPointsOlderThanCutoff) {
    const auto dbPath = uniqueDbPath("retention");
    const auto root = dbPath.parent_path();
    {
        engine::RiskDb db(dbPath.string());
        db.insertEquityPoint(engine::EquityPoint{
            .timestampMs = 1000,
            .equity = 100.0,
            .year = 2026,
            .source = "periodic",
            .basis = "margin",
        });
        db.insertEquityPoint(engine::EquityPoint{
            .timestampMs = 2000,
            .equity = 101.0,
            .year = 2026,
            .source = "periodic",
            .basis = "margin",
        });
        db.insertEquityPoint(engine::EquityPoint{
            .timestampMs = 3000,
            .equity = 102.0,
            .year = 2026,
            .source = "periodic",
            .basis = "margin",
        });

        db.deleteEquityPointsOlderThan(2500);
        const auto kept = db.getByTimeRange("margin", 0, 10'000);
        ASSERT_EQ(kept.size(), 1u);
        EXPECT_EQ(kept.front().timestampMs, 3000);
    }

    std::filesystem::remove_all(root);
}

} // namespace
