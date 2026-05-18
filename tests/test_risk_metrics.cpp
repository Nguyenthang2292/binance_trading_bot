#include <gtest/gtest.h>

#include "risk/risk_metrics.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path sp500FixturePath() {
    std::filesystem::path p =
        std::filesystem::path(BOT_SOURCE_DIR) / "tests" / "fixtures" / "sp500_1992_2019_close.csv";
    return p;
}

std::vector<std::pair<int, double>> loadSp500Closes() {
    const auto path = sp500FixturePath();
    std::ifstream in(path);
    if (!in.is_open()) {
        throw std::runtime_error("cannot open fixture: " + path.string());
    }
    std::string line;
    std::getline(in, line); // header

    std::vector<std::pair<int, double>> out;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::stringstream ss(line);
        std::string yearText;
        std::string closeText;
        if (!std::getline(ss, yearText, ',')) {
            continue;
        }
        if (!std::getline(ss, closeText, ',')) {
            continue;
        }
        out.emplace_back(std::stoi(yearText), std::stod(closeText));
    }
    return out;
}

TEST(RiskMetricsTest, ReturnsInvalidWhenDataPointsBelowConfiguredMinimum) {
    engine::RiskMetrics metrics(0.0, 10, std::chrono::minutes{60});
    std::vector<engine::SampledEquityPoint> points{
        {.timestampMs = 0, .equity = 100.0},
        {.timestampMs = 3'600'000, .equity = 101.0},
        {.timestampMs = 7'200'000, .equity = 102.0},
    };

    const auto result = metrics.compute(points, "rolling", 0, 7'200'000, "margin");
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.dataPoints, 3);
}

TEST(RiskMetricsTest, UsesInfinityWhenNoDownsideReturnsOrDrawdowns) {
    engine::RiskMetrics metrics(0.0, 5, std::chrono::minutes{60});
    std::vector<engine::SampledEquityPoint> points;
    points.reserve(40);
    double equity = 100.0;
    int64_t ts = 0;
    for (int i = 0; i < 40; ++i) {
        points.push_back(engine::SampledEquityPoint{.timestampMs = ts, .equity = equity});
        ts += 3'600'000;
        equity *= 1.01;
    }

    const auto result = metrics.compute(points, "rolling", 0, ts - 3'600'000, "margin");
    EXPECT_TRUE(result.valid);
    EXPECT_GT(result.annualReturn, 0.0);
    EXPECT_DOUBLE_EQ(result.stdDevDownside, 0.0);
    EXPECT_TRUE(std::isinf(result.sortinoRatio));
    EXPECT_DOUBLE_EQ(result.maxDrawdown, 0.0);
    EXPECT_TRUE(std::isinf(result.upi));
}

TEST(RiskMetricsTest, AnnualizedReturnMatchesCagrByElapsedTime) {
    engine::RiskMetrics metrics(0.0, 2, std::chrono::minutes{60 * 24});
    std::vector<engine::SampledEquityPoint> points{
        {.timestampMs = 0, .equity = 100.0},
        {.timestampMs = 365LL * 24 * 60 * 60 * 1000, .equity = 121.0},
    };

    const auto result = metrics.compute(points, "rolling", 0, points.back().timestampMs, "margin");
    ASSERT_TRUE(result.valid);
    EXPECT_NEAR(result.annualReturn, 0.21, 1e-6);
}

TEST(RiskMetricsTest, UlcerIndexUsesDecimalUnitsNotPercentUnits) {
    engine::RiskMetrics metrics(0.0, 2, std::chrono::minutes{60 * 24});
    std::vector<engine::SampledEquityPoint> points;
    points.reserve(20);
    points.push_back({.timestampMs = 0, .equity = 100.0});
    for (int i = 1; i < 20; ++i) {
        points.push_back({
            .timestampMs = static_cast<int64_t>(i) * 24 * 60 * 60 * 1000,
            .equity = 85.124, // drawdown ~= -0.14876 from peak 100
        });
    }

    const auto result = metrics.compute(points, "rolling", 0, points.back().timestampMs, "margin");
    ASSERT_TRUE(result.valid);
    EXPECT_NEAR(result.ulcerIndex, 0.145, 1e-3);
    EXPECT_LT(result.ulcerIndex, 1.0);
}

TEST(RiskMetricsTest, Sp5001992To2019FixtureMatchesBookUlcerIndexGroundTruth) {
    const auto rows = loadSp500Closes();
    ASSERT_EQ(rows.size(), 28u);
    EXPECT_EQ(rows.front().first, 1992);
    EXPECT_EQ(rows.back().first, 2019);

    std::vector<engine::SampledEquityPoint> points;
    points.reserve(rows.size());
    int64_t ts = 0;
    constexpr int64_t kYearMs = 365LL * 24 * 60 * 60 * 1000;
    for (const auto& [year, close] : rows) {
        (void)year;
        points.push_back(engine::SampledEquityPoint{.timestampMs = ts, .equity = close});
        ts += kYearMs;
    }

    engine::RiskMetrics metrics(0.0, 2, std::chrono::minutes{60 * 24 * 365});
    const auto result = metrics.compute(points, "calendar_year", 0, points.back().timestampMs, "margin");

    ASSERT_TRUE(result.valid);
    // Ground truth from book table 7.1: UI = 14.5% for SP500 (1992-2019).
    EXPECT_NEAR(result.ulcerIndex, 0.145, 0.001);
    EXPECT_NEAR(result.maxDrawdown, -0.4012, 0.001);
}

} // namespace
