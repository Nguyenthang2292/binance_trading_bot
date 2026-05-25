#pragma once

#include "types/market.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace backtest {

class IKlineRestClient {
public:
    virtual ~IKlineRestClient() = default;

    struct FetchResult {
        bool success{false};
        std::vector<Kline> bars;
        int pagesUsed{0};
        std::chrono::milliseconds wallTime{0};
        std::string errorMessage{};
    };

    virtual FetchResult fetchClosedKlines(
        std::string_view symbol,
        std::string_view interval,
        long long signalOpenMs,
        int limit,
        std::chrono::milliseconds timeout,
        int maxRequests) const = 0;
};

}  // namespace backtest
