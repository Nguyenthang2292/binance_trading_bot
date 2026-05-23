#include "backtest/walk_forward.h"

#include <algorithm>
#include <cmath>

namespace backtest {

Partitions PartitionBuilder::build(
    const std::vector<Kline>& closedKlines,
    double promptContextFraction) {

    Partitions result;
    result.valid = false;

    if (closedKlines.empty()) {
        return result;
    }

    // Signal bar T = last closed bar
    const std::size_t signalIdx = closedKlines.size() - 1;
    result.signalBar = closedKlines[signalIdx];

    if (signalIdx == 0) {
        return result;  // only signal bar — no room for prompt/calibration
    }

    const double frac = std::clamp(promptContextFraction, 0.0, 1.0);
    const std::size_t available = signalIdx;  // everything before T
    std::size_t promptSize = static_cast<std::size_t>(std::floor(available * frac));

    // Guarantee at least 1 bar in each partition when we have ≥ 2 available.
    if (promptSize == 0 && available >= 2) promptSize = 1;
    if (promptSize >= available) promptSize = available - 1;

    result.promptContext.reserve(promptSize);
    result.calibrationWindow.reserve(available - promptSize);

    for (std::size_t i = 0; i < promptSize; ++i) {
        result.promptContext.push_back(closedKlines[i]);
    }
    for (std::size_t i = promptSize; i < signalIdx; ++i) {
        result.calibrationWindow.push_back(closedKlines[i]);
    }

    result.valid = !result.calibrationWindow.empty() && !result.promptContext.empty();
    return result;
}

std::vector<WalkForwardFold> WalkForwardSplitter::split(
    const std::vector<Kline>& calibrationWindow,
    int numFolds,
    double isFraction) {

    std::vector<WalkForwardFold> folds;
    if (calibrationWindow.empty() || numFolds <= 0 || isFraction <= 0.0 || isFraction >= 1.0) {
        return folds;
    }

    const std::size_t N = calibrationWindow.size();

    // We carve `numFolds` contiguous OOS slices from the END of the window.
    // The first IS occupies the initial chunk; each subsequent fold's IS grows
    // by oosSize (anchored-expanding) so fold k+1's IS never overlaps any prior
    // fold's OOS.
    //
    // Let oos = OOS slice size, B = initial IS blocks (each of size oos).
    // Then total = (B + numFolds) * oos = N  =>  oos = N / (B + numFolds).
    // isFraction = B / (B + 1)  (IS share of fold 0 = isFraction of IS+OOS_first)
    // =>  B = isFraction / (1 - isFraction).
    const double bExact = isFraction / (1.0 - isFraction);
    const std::size_t bBlocks = std::max<std::size_t>(1, static_cast<std::size_t>(std::round(bExact)));
    const std::size_t oosSize = std::max<std::size_t>(1, N / (bBlocks + static_cast<std::size_t>(numFolds)));

    // Verify we have enough bars; if not, return empty so caller can drop with InsufficientData.
    const std::size_t needed = (bBlocks + static_cast<std::size_t>(numFolds)) * oosSize;
    if (needed > N) {
        return folds;
    }

    const std::size_t startOffset = N - needed;  // skip stale prefix if any
    folds.reserve(static_cast<std::size_t>(numFolds));

    for (int k = 0; k < numFolds; ++k) {
        WalkForwardFold fold;

        // IS goes from startOffset to start of this fold's OOS.
        // For fold k: IS = [startOffset, startOffset + (bBlocks + k) * oosSize)
        //             OOS = [end of IS, end of IS + oosSize)
        const std::size_t isStart = startOffset;
        const std::size_t isEnd   = startOffset + (bBlocks + static_cast<std::size_t>(k)) * oosSize;
        const std::size_t oosEnd  = isEnd + oosSize;

        if (oosEnd > N) break;  // safety; shouldn't trigger given size check above

        fold.inSample.assign(
            calibrationWindow.begin() + static_cast<std::ptrdiff_t>(isStart),
            calibrationWindow.begin() + static_cast<std::ptrdiff_t>(isEnd));
        fold.outOfSample.assign(
            calibrationWindow.begin() + static_cast<std::ptrdiff_t>(isEnd),
            calibrationWindow.begin() + static_cast<std::ptrdiff_t>(oosEnd));

        folds.push_back(std::move(fold));
    }

    return folds;
}

}  // namespace backtest
