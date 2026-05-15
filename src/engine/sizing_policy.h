#pragma once

namespace engine {

struct SizingInput {
    double availableBalance{0.0};
    double atr{0.0};
    double riskPct{0.0};
    double slMultiplier{0.0};
    double minNotional{0.0};
};

struct SizingResult {
    double notional{0.0};
    double quantity{0.0};
    bool isMinClamped{false};
};

SizingResult calculateSize(const SizingInput& input, double currentPrice, double stepSize);

} // namespace engine

