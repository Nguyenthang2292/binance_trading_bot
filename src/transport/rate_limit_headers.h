#pragma once

#include <string_view>

struct RateLimitHeaderUsage {
    int usedWeight1m{-1};
    int usedOrders1m{-1};
    int usedOrders10s{-1};
};

void applyRateLimitHeader(RateLimitHeaderUsage& usage,
                          std::string_view name,
                          std::string_view value);
