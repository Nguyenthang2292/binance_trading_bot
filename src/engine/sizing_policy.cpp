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

    const double riskCapital = input.availableBalance * input.riskPct;
    const double rawQty = riskCapital / (input.atr * input.slMultiplier);
    const double rawNotional = rawQty * currentPrice;
    out.isMinClamped = rawNotional < input.minNotional;
    double targetNotional = std::max(input.minNotional, rawNotional);
    if (input.maxNotional > 0.0 && targetNotional > input.maxNotional) {
        targetNotional = input.maxNotional;
        out.isMaxCapped = true;
    }
    if (targetNotional < input.minNotional) {
        return out;
    }
    out.notional = targetNotional;

    const double targetQty = out.notional / currentPrice;
    double steps = std::floor(targetQty / stepSize);
    out.quantity = std::max(0.0, steps * stepSize);
    if (out.quantity * currentPrice < input.minNotional) {
        steps = std::ceil((input.minNotional / currentPrice) / stepSize);
        out.quantity = std::max(0.0, steps * stepSize);
    }
    if (input.maxNotional > 0.0 && out.quantity * currentPrice > input.maxNotional) {
        out.quantity = 0.0;
        out.notional = 0.0;
        out.isMaxCapped = true;
        return out;
    }
    out.notional = out.quantity * currentPrice;
    return out;
}

} // namespace engine

