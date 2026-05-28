/**
 * @file rest_client_kline_adapter.h
 * @brief Adapter and pagination helpers for Binance REST kline backfill.
 */

#pragma once

#include "backtest/ikline_rest_client.h"
#include "types/market.h"

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

class RestClient;

namespace backtest {

namespace detail {

constexpr int kBinanceKlinePageMax = 1500;

struct PageOutcome {
    bool ok{false};
    std::vector<Kline> bars;          // expected ascending by openTime (Binance native order)
    std::string error;                // empty when ok; "timeout" reserved for wall-clock loss
};

// Page-level fetch callback. `endTimeMs` is the inclusive close-time anchor for the page,
// `timeout` is the remaining wall-clock budget for this single page (0 means no timeout).
using PageFetcher = std::function<PageOutcome(
    std::string_view symbol,
    std::string_view interval,
    int limit,
    long long endTimeMs,
    std::chrono::milliseconds timeout)>;

// Pure pagination/off-by-one/budget/timeout/filter logic; no asio/network dependency.
// Exposed in the header so unit tests can inject a fake `PageFetcher`.
IKlineRestClient::FetchResult paginateClosedKlines(
    const PageFetcher& fetcher,
    std::string_view symbol,
    std::string_view interval,
    long long signalOpenMs,
    int limit,
    std::chrono::milliseconds timeout,
    int maxRequests);

std::optional<int64_t> parseIntervalMs(std::string_view interval);

}  // namespace detail

class RestClientKlineAdapter final : public IKlineRestClient {
public:
    /**
     * @brief Wraps an async RestClient and io_context into IKlineRestClient API.
     */
    RestClientKlineAdapter(RestClient& restClient, boost::asio::io_context& ioc);

    /**
     * @brief Fetches closed klines with page budget and timeout controls.
     */
    FetchResult fetchClosedKlines(
        std::string_view symbol,
        std::string_view interval,
        long long signalOpenMs,
        int limit,
        std::chrono::milliseconds timeout,
        int maxRequests) const override;

private:
    RestClient& m_restClient;
    boost::asio::io_context& m_ioc;
};

}  // namespace backtest
