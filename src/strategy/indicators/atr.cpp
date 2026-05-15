#include "strategy/indicators/atr.h"

#include <algorithm>
#include <cmath>

namespace strategy::indicators {

std::vector<double> atr(const std::vector<Kline>& klines, int period) {
    std::vector<double> out(klines.size(), 0.0);
    if (period <= 0 || klines.size() < static_cast<size_t>(period + 1)) {
        return out;
    }

    std::vector<double> tr(klines.size(), 0.0);
    for (size_t i = 1; i < klines.size(); ++i) {
        const auto a = klines[i].high - klines[i].low;
        const auto b = std::abs(klines[i].high - klines[i - 1].close);
        const auto c = std::abs(klines[i].low - klines[i - 1].close);
        tr[i] = std::max({a, b, c});
    }

    double seed = 0.0;
    for (int i = 1; i <= period; ++i) {
        seed += tr[static_cast<size_t>(i)];
    }
    double prev = seed / static_cast<double>(period);
    out[static_cast<size_t>(period)] = prev;

    for (size_t i = static_cast<size_t>(period + 1); i < klines.size(); ++i) {
        prev = ((prev * static_cast<double>(period - 1)) + tr[i]) / static_cast<double>(period);
        out[i] = prev;
    }

    return out;
}

double lastAtr(const std::vector<Kline>& klines, int period) {
    const auto values = atr(klines, period);
    if (values.empty()) {
        return 0.0;
    }
    return values.back();
}

} // namespace strategy::indicators

