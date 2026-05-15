#include "engine/sizing_policy.h"

#include <algorithm>
#include <cmath>

namespace engine {

SizingResult calculateSize(const SizingInput& input, double currentPrice, double stepSize) {
    SizingResult out;
    if (input.atr <= 0.0 || input.slMultiplier <= 0.0 || input.riskPct <= 0.0 || currentPrice <= 0.0 ||
        stepSize <= 0.0 || input.availableBalance <= 0.0) {
        return out;
    }

    const double rawNotional = input.availableBalance * input.riskPct / (input.atr * input.slMultiplier);
    out.isMinClamped = rawNotional < input.minNotional;
    out.notional = std::max(input.minNotional, rawNotional);

    const double rawQty = out.notional / currentPrice;
    const double steps = std::floor(rawQty / stepSize);
    out.quantity = std::max(0.0, steps * stepSize);
    return out;
}

} // namespace engine

