#include "strategy/indicators/atr.h"

#include <algorithm>
#include <cmath>

namespace strategy::indicators {

namespace {

bool isValidOhlc(const Kline& kline) {
    return std::isfinite(kline.high) &&
           std::isfinite(kline.low) &&
           std::isfinite(kline.close) &&
           kline.high >= kline.low;
}

double trueRange(const Kline& current, const Kline& previous) {
    const auto highLow = current.high - current.low;
    const auto highPrevClose = std::abs(current.high - previous.close);
    const auto lowPrevClose = std::abs(current.low - previous.close);
    const auto tr = std::max({highLow, highPrevClose, lowPrevClose});
    return std::isfinite(tr) && tr >= 0.0 ? tr : 0.0;
}

} // namespace

std::vector<double> atr(const std::vector<Kline>& klines, int period) {
    std::vector<double> out(klines.size(), 0.0);
    if (period <= 0) {
        return out;
    }
    const auto window = static_cast<size_t>(period);
    if (klines.size() <= window) {
        return out;
    }

    for (const auto& kline : klines) {
        if (!isValidOhlc(kline)) {
            return out;
        }
    }

    std::vector<double> tr(klines.size(), 0.0);
    for (size_t i = 1; i < klines.size(); ++i) {
        tr[i] = trueRange(klines[i], klines[i - 1]);
    }

    double seed = 0.0;
    for (size_t i = 1; i <= window; ++i) {
        seed += tr[i];
    }
    double prev = seed / static_cast<double>(period);
    out[window] = prev;

    for (size_t i = window + 1; i < klines.size(); ++i) {
        prev = ((prev * static_cast<double>(period - 1)) + tr[i]) / static_cast<double>(period);
        out[i] = prev;
    }

    return out;
}

double lastAtr(const std::vector<Kline>& klines, int period) {
    if (period <= 0) {
        return 0.0;
    }
    const auto window = static_cast<size_t>(period);
    if (klines.size() <= window) {
        return 0.0;
    }

    for (const auto& kline : klines) {
        if (!isValidOhlc(kline)) {
            return 0.0;
        }
    }

    double seed = 0.0;
    for (size_t i = 1; i <= window; ++i) {
        seed += trueRange(klines[i], klines[i - 1]);
    }
    double prev = seed / static_cast<double>(period);

    for (size_t i = window + 1; i < klines.size(); ++i) {
        prev = ((prev * static_cast<double>(period - 1)) + trueRange(klines[i], klines[i - 1])) /
               static_cast<double>(period);
    }
    return std::isfinite(prev) ? prev : 0.0;
}

} // namespace strategy::indicators

