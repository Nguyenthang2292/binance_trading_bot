#include "backtest/plateau_finder.h"

#include <gtest/gtest.h>

using namespace backtest;

namespace {
ScoredPoint makeSP(double maVal, double oosSortino, bool passed) {
    ScoredPoint sp;
    sp.point = {{"ma", maVal}};
    sp.oosSortino = oosSortino;
    sp.isSortino = oosSortino;
    sp.passedFilters = passed;
    return sp;
}
}  // namespace

TEST(PlateauFinderTest, EmptyGridReturnsNullopt) {
    auto r = PlateauFinder::find({}, {}, 1, 10, 0.5);
    EXPECT_FALSE(r.has_value());
}

TEST(PlateauFinderTest, IsolatedPeakRejected) {
    // Best is sharply isolated: neighbors fail.
    std::vector<ScoredPoint> grid = {
        makeSP(10, 0.1, false),
        makeSP(15, 0.1, false),
        makeSP(20, 2.0, true),     // peak
        makeSP(25, 0.1, false),
        makeSP(30, 0.1, false),
    };
    auto r = PlateauFinder::find(grid, {}, 1, 10, 0.6);
    EXPECT_FALSE(r.has_value());
}

TEST(PlateauFinderTest, BroadPlateauAccepted) {
    // 5 contiguous winners → broad plateau. Best (highest OOS) is at ma=20.
    std::vector<ScoredPoint> grid = {
        makeSP(10, 1.0, true),
        makeSP(15, 1.1, true),
        makeSP(20, 1.5, true),     // best
        makeSP(25, 1.1, true),
        makeSP(30, 1.0, true),
    };
    auto r = PlateauFinder::find(grid, {}, 1, 10, 0.5);
    ASSERT_TRUE(r.has_value());
    // Survivors = {ma=15, ma=20, ma=25}, mean = 20, snap → 20.
    EXPECT_DOUBLE_EQ(r->center.at("ma"), 20.0);
}

TEST(PlateauFinderTest, ConstraintRevalidation) {
    // 2D grid, ensure snapped center still respects ma_short < ma_long.
    std::vector<ScoredPoint> grid;
    for (int s : {10, 20}) {
        for (int l : {30, 40}) {
            ScoredPoint sp;
            sp.point = {{"ma_short", static_cast<double>(s)},
                        {"ma_long",  static_cast<double>(l)}};
            sp.oosSortino = 1.0;
            sp.isSortino = 1.0;
            sp.passedFilters = true;
            grid.push_back(sp);
        }
    }
    std::vector<ParamConstraint> cons = {
        {"ma_short", ParamConstraint::Kind::LessThan, "ma_long"}
    };
    auto r = PlateauFinder::find(grid, cons, 1, 10, 0.5);
    ASSERT_TRUE(r.has_value());
    EXPECT_LT(r->center.at("ma_short"), r->center.at("ma_long"));
}

TEST(PlateauFinderTest, InactiveDimensionUsesGridIndexNotFloatEquality) {
    std::vector<ScoredPoint> grid;
    for (double ma : {10.0, 20.0, 30.0}) {
        ScoredPoint sp;
        sp.point = {
            {"ma", ma},
            {"noise", ma == 20.0 ? 0.30000000000000004 : 0.3},
            {"aux", 1.0},
        };
        sp.oosSortino = (ma == 20.0) ? 2.0 : 1.0;
        sp.isSortino = sp.oosSortino;
        sp.passedFilters = true;
        grid.push_back(sp);
    }

    auto r = PlateauFinder::find(
        grid,
        {},
        /*neighborhoodRadius=*/1,
        /*maxNeighborhoodSize=*/3,
        /*minPassFraction=*/0.5);
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(r->center.at("ma"), 20.0);
}
