/**
 * @file ikline_rest_client.h
 * @brief Interface for REST-based closed-kline backfill.
 */

#pragma once

#include "types/market.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace backtest {

/**
 * @brief Abstract REST client used by backtest providers for runtime backfill.
 */
class IKlineRestClient {
public:
    virtual ~IKlineRestClient() = default;

    /**
     * @brief Result envelope for REST kline fetch.
     */
    struct FetchResult {
        bool success{false};   ///< True when fetch completed successfully.
        std::vector<Kline> bars;   ///< Closed bars in ascending open-time order.
        int pagesUsed{0};      ///< Number of REST pages consumed.
        std::chrono::milliseconds wallTime{0};   ///< End-to-end wall time.
        std::string errorMessage{};  ///< Structured failure reason when !success.
    };

    /**
     * @brief Fetches up to `limit` closed bars ending at signal bar T.
     */
    virtual FetchResult fetchClosedKlines(
        std::string_view symbol,
        std::string_view interval,
        long long signalOpenMs,
        int limit,
        std::chrono::milliseconds timeout,
        int maxRequests) const = 0;
};

}  // namespace backtest
