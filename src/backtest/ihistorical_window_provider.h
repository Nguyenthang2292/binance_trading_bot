#pragma once

#include "types/market.h"

#include <chrono>
#include <string_view>
#include <vector>

namespace backtest {

class IHistoricalWindowProvider {
public:
    virtual ~IHistoricalWindowProvider() = default;

    struct WindowResult {
        bool sufficient{false};
        int requiredBars{0};
        int availableBars{0};
        std::vector<Kline> closedKlines;
    };

    virtual WindowResult closedWindow(
        std::string_view symbol,
        std::string_view interval,
        int requiredClosedBars,
        std::chrono::system_clock::time_point signalBarOpenTime) const = 0;
};

}  // namespace backtest
