/**
 * @file walk_forward.h
 * @brief Partitioning and walk-forward fold construction utilities.
 */

#pragma once

#include "types/market.h"

#include <vector>

namespace backtest {

/**
 * @brief Prompt/calibration/signal partition of a closed-kline window.
 */
struct Partitions {
    std::vector<Kline> promptContext;
    std::vector<Kline> calibrationWindow;
    Kline signalBar{};
    bool valid{false};  // false if input size insufficient for both partitions
};

/**
 * @brief One walk-forward fold with IS and OOS segments.
 */
struct WalkForwardFold {
    std::vector<Kline> inSample;
    std::vector<Kline> outOfSample;
};

// Splits closed klines into [promptContext][calibrationWindow][signalBar].
// `promptContextFraction` is the fraction of (closedKlines.size() - 1) reserved
// for the prompt context; the remainder forms the calibration window. The very
// last bar in closedKlines is the signal bar T.
class PartitionBuilder {
public:
    /**
     * @brief Splits closed bars into prompt context, calibration, and signal bar.
     */
    static Partitions build(
        const std::vector<Kline>& closedKlines,
        double promptContextFraction);
};

// Generates anchored-expanding walk-forward folds over `calibrationWindow`.
//
// Anchored expanding means: each fold's IS is [start..end_i], and the OOS is
// the next contiguous slice. Successive folds extend IS forward without ever
// reusing a previous fold's OOS as IS — preventing look-ahead bias.
//
// Concretely, with N = calibrationWindow.size(), `numFolds` OOS slices of
// size oosSize = N / (numFolds + initialIsBlocks) are carved out from the end.
// initialIsBlocks is derived from isFraction: IS_total / OOS_per_fold.
// The first fold's IS starts at the beginning; later folds' IS grows by oosSize.
class WalkForwardSplitter {
public:
    /**
     * @brief Builds anchored-expanding walk-forward folds on calibration window.
     */
    static std::vector<WalkForwardFold> split(
        const std::vector<Kline>& calibrationWindow,
        int numFolds,
        double isFraction);
};

}  // namespace backtest
