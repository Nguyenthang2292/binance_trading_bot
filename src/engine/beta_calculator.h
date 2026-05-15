#pragma once

#include "scanner/kline_cache.h"

#include <optional>
#include <string_view>
#include <vector>

namespace engine {

class BetaCalculator {
public:
    std::optional<double> calculate(
        std::string_view symbol,
        const scanner::KlineCache& cache,
        int windowDays) const;

private:
    static double ols(const std::vector<double>& coinReturns, const std::vector<double>& btcReturns);
};

} // namespace engine
