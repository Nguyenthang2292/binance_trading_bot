#include "engine/beta_calculator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

namespace engine {

namespace {

constexpr double kMinVariance = 1e-12;

std::vector<double> tail(const std::vector<double>& in, size_t n) {
    if (n >= in.size()) {
        return in;
    }
    return std::vector<double>(in.end() - static_cast<std::ptrdiff_t>(n), in.end());
}

} // namespace

std::optional<double> BetaCalculator::calculate(
    std::string_view symbol,
    const scanner::KlineCache& cache,
    int windowDays) const {
    const auto coinKlines = cache.snapshot(symbol, "1d");
    const auto btcKlines = cache.snapshot("BTCUSDT", "1d");
    if (!coinKlines || !btcKlines) {
        return std::nullopt;
    }

    std::map<int64_t, double> coinCloseByOpenTime;
    std::map<int64_t, double> btcCloseByOpenTime;
    for (const auto& k : *coinKlines) {
        coinCloseByOpenTime[k.openTime] = k.close;
    }
    for (const auto& k : *btcKlines) {
        btcCloseByOpenTime[k.openTime] = k.close;
    }

    std::vector<int64_t> commonTimes;
    commonTimes.reserve(std::min(coinCloseByOpenTime.size(), btcCloseByOpenTime.size()));
    auto coinIt = coinCloseByOpenTime.begin();
    auto btcIt = btcCloseByOpenTime.begin();
    while (coinIt != coinCloseByOpenTime.end() && btcIt != btcCloseByOpenTime.end()) {
        if (coinIt->first == btcIt->first) {
            commonTimes.push_back(coinIt->first);
            ++coinIt;
            ++btcIt;
        } else if (coinIt->first < btcIt->first) {
            ++coinIt;
        } else {
            ++btcIt;
        }
    }

    if (commonTimes.size() < 2) {
        return std::nullopt;
    }

    std::vector<double> coinReturns;
    std::vector<double> btcReturns;
    coinReturns.reserve(commonTimes.size() - 1);
    btcReturns.reserve(commonTimes.size() - 1);
    for (size_t i = 1; i < commonTimes.size(); ++i) {
        const auto prevTime = commonTimes[i - 1];
        const auto nowTime = commonTimes[i];
        const double coinPrev = coinCloseByOpenTime[prevTime];
        const double coinNow = coinCloseByOpenTime[nowTime];
        const double btcPrev = btcCloseByOpenTime[prevTime];
        const double btcNow = btcCloseByOpenTime[nowTime];
        if (coinPrev <= 0.0 || coinNow <= 0.0 || btcPrev <= 0.0 || btcNow <= 0.0) {
            continue;
        }
        coinReturns.push_back((coinNow - coinPrev) / coinPrev);
        btcReturns.push_back((btcNow - btcPrev) / btcPrev);
    }

    if (coinReturns.size() < 2 || btcReturns.size() < 2) {
        return std::nullopt;
    }

    const size_t window = windowDays > 0 ? static_cast<size_t>(windowDays) : 0;
    if (window > 0) {
        const size_t n = std::min(window, std::min(coinReturns.size(), btcReturns.size()));
        if (n < 2) {
            return std::nullopt;
        }
        coinReturns = tail(coinReturns, n);
        btcReturns = tail(btcReturns, n);
    }

    const double beta = ols(coinReturns, btcReturns);
    if (!std::isfinite(beta)) {
        return std::nullopt;
    }
    return beta;
}

double BetaCalculator::ols(const std::vector<double>& coinReturns, const std::vector<double>& btcReturns) {
    const size_t n = std::min(coinReturns.size(), btcReturns.size());
    if (n < 2) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const double muX = std::accumulate(btcReturns.begin(), btcReturns.begin() + static_cast<std::ptrdiff_t>(n), 0.0) /
        static_cast<double>(n);
    const double muY = std::accumulate(coinReturns.begin(), coinReturns.begin() + static_cast<std::ptrdiff_t>(n), 0.0) /
        static_cast<double>(n);

    double cov = 0.0;
    double var = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double dx = btcReturns[i] - muX;
        const double dy = coinReturns[i] - muY;
        cov += dx * dy;
        var += dx * dx;
    }
    cov /= static_cast<double>(n - 1);
    var /= static_cast<double>(n - 1);

    if (std::abs(var) < kMinVariance) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return cov / var;
}

} // namespace engine
