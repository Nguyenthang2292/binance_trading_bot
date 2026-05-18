#include <gtest/gtest.h>

#include "risk/risk_metrics.h"
#include "risk/risk_types.h"

#include <chrono>
#include <vector>

namespace {

TEST(RiskSamplingTest, SamplesLatestPerBucketWithDeterministicTieBreaks) {
    std::vector<engine::EquityPoint> raw;
    raw.push_back(engine::EquityPoint{
        .id = 1,
        .timestampMs = 1000,
        .equity = 100.0,
        .year = 2026,
        .source = "periodic",
        .basis = "margin",
    });
    raw.push_back(engine::EquityPoint{
        .id = 2,
        .timestampMs = 2000,
        .equity = 101.0,
        .year = 2026,
        .source = "trade_close",
        .basis = "margin",
    });
    raw.push_back(engine::EquityPoint{
        .id = 3,
        .timestampMs = 3'601'000,
        .equity = 102.0,
        .year = 2026,
        .source = "periodic",
        .basis = "margin",
    });
    raw.push_back(engine::EquityPoint{
        .id = 4,
        .timestampMs = 3'601'000,
        .equity = 103.0,
        .year = 2026,
        .source = "trade_close",
        .basis = "margin",
    });
    raw.push_back(engine::EquityPoint{
        .id = 5,
        .timestampMs = 3'601'000,
        .equity = 104.0,
        .year = 2026,
        .source = "trade_close",
        .basis = "margin",
    });

    const auto sampled = engine::sampleEquity(
        raw,
        0,
        7'200'000,
        std::chrono::minutes{60});

    ASSERT_EQ(sampled.size(), 2u);
    EXPECT_EQ(sampled[0].timestampMs, 2000);
    EXPECT_DOUBLE_EQ(sampled[0].equity, 101.0);
    EXPECT_EQ(sampled[1].timestampMs, 3'601'000);
    EXPECT_DOUBLE_EQ(sampled[1].equity, 104.0);
}

} // namespace
