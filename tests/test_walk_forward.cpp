#include "backtest/walk_forward.h"

#include <gtest/gtest.h>

using namespace backtest;

namespace {
std::vector<Kline> makeRange(int n) {
    std::vector<Kline> klines;
    klines.reserve(n);
    for (int i = 0; i < n; ++i) {
        Kline k{};
        k.openTime = i;
        k.closeTime = i + 1;
        k.open = k.close = 100.0;
        k.high = 101.0;
        k.low = 99.0;
        k.isClosed = true;
        klines.push_back(k);
    }
    return klines;
}
}  // namespace

TEST(PartitionBuilderTest, ProducesThreeSegmentsWithCorrectBoundaries) {
    auto klines = makeRange(101);            // 100 pre-signal + 1 signal
    auto parts  = PartitionBuilder::build(klines, 0.5);

    ASSERT_TRUE(parts.valid);
    EXPECT_EQ(parts.signalBar.openTime, 100);
    EXPECT_EQ(parts.promptContext.size(), 50u);
    EXPECT_EQ(parts.calibrationWindow.size(), 50u);

    // Prompt covers indices [0..49]
    EXPECT_EQ(parts.promptContext.front().openTime, 0);
    EXPECT_EQ(parts.promptContext.back().openTime, 49);
    // Calibration covers [50..99]
    EXPECT_EQ(parts.calibrationWindow.front().openTime, 50);
    EXPECT_EQ(parts.calibrationWindow.back().openTime, 99);
}

TEST(PartitionBuilderTest, EmptyInputReturnsInvalid) {
    auto parts = PartitionBuilder::build({}, 0.5);
    EXPECT_FALSE(parts.valid);
}

TEST(PartitionBuilderTest, SingleBarReturnsInvalid) {
    auto parts = PartitionBuilder::build(makeRange(1), 0.5);
    EXPECT_FALSE(parts.valid);
}

TEST(WalkForwardSplitterTest, FoldsRespectAnchoredExpandingInvariants) {
    auto cal = makeRange(100);
    auto folds = WalkForwardSplitter::split(cal, /*numFolds=*/4, /*isFraction=*/0.75);

    ASSERT_EQ(folds.size(), 4u);

    // (1) OOS slices must not overlap each other.
    for (std::size_t i = 0; i + 1 < folds.size(); ++i) {
        EXPECT_LT(folds[i].outOfSample.back().openTime,
                  folds[i + 1].outOfSample.front().openTime);
    }

    // (2) Within a single fold, the IS must precede its own OOS — no
    //     within-fold look-ahead. (In anchored-expanding WF, IS of fold k+1
    //     deliberately INCLUDES the OOS bars of fold k; this is correct and
    //     NOT a look-ahead violation because each fold is a fresh
    //     optimization session and that fold's own OOS is never seen by its
    //     own IS.)
    for (const auto& f : folds) {
        ASSERT_FALSE(f.inSample.empty());
        ASSERT_FALSE(f.outOfSample.empty());
        EXPECT_LT(f.inSample.back().openTime, f.outOfSample.front().openTime);
    }

    // (3) A fold's IS must NEVER contain its own OOS.
    for (const auto& f : folds) {
        for (const auto& isBar : f.inSample) {
            for (const auto& oosBar : f.outOfSample) {
                EXPECT_NE(isBar.openTime, oosBar.openTime);
            }
        }
    }

    // (4) Anchored-expanding monotonicity: |IS_k| <= |IS_{k+1}|.
    for (std::size_t i = 0; i + 1 < folds.size(); ++i) {
        EXPECT_LE(folds[i].inSample.size(), folds[i + 1].inSample.size());
    }
}

TEST(WalkForwardSplitterTest, RejectsInvalidArgs) {
    EXPECT_TRUE(WalkForwardSplitter::split({}, 4, 0.75).empty());
    EXPECT_TRUE(WalkForwardSplitter::split(makeRange(20), 0, 0.75).empty());
    EXPECT_TRUE(WalkForwardSplitter::split(makeRange(20), 4, 0.0).empty());
    EXPECT_TRUE(WalkForwardSplitter::split(makeRange(20), 4, 1.0).empty());
}
