/**
 * @file ihistorical_window_provider.h
 * @brief Interface for retrieving a closed historical kline window at bar T.
 */

#pragma once

#include "types/market.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace backtest {

/**
 * @brief Abstract provider for closed historical window retrieval.
 */
class IHistoricalWindowProvider {
public:
    virtual ~IHistoricalWindowProvider() = default;

    /**
     * @brief Result envelope for closed-window retrieval attempts.
     */
    struct WindowResult {
        bool sufficient{false};   ///< True when `closedKlines` satisfies request.
        int requiredBars{0};      ///< Requested closed-bar count.
        int availableBars{0};     ///< Bars available from selected source.
        std::vector<Kline> closedKlines;   ///< Closed bars ending at signal bar T.
        std::string source{};     ///< Source label, for example "cache" or "rest".
        int restPagesUsed{0};     ///< REST pages consumed when source uses backfill.
        std::chrono::milliseconds restWallTimeMs{0};   ///< REST wall-time spent.
        std::string errorReason{};   ///< Source-specific failure reason.
    };

    /**
     * @brief Fetches closed bars up to and including `signalBarOpenTime`.
     */
    virtual WindowResult closedWindow(
        std::string_view symbol,
        std::string_view interval,
        int requiredClosedBars,
        std::chrono::system_clock::time_point signalBarOpenTime) const = 0;
};

}  // namespace backtest
