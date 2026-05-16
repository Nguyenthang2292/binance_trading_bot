#include "engine/price_filter.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace engine {
namespace {

std::string fixedTrimmed(double value, int decimals) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(decimals) << value;
    std::string text = out.str();
    const auto dot = text.find('.');
    if (dot != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text.empty() ? "0" : text;
}

int decimalsForTick(double tickSize) {
    if (tickSize <= 0.0 || !std::isfinite(tickSize)) {
        return 16;
    }

    const std::string tickText = fixedTrimmed(tickSize, 16);
    const auto dot = tickText.find('.');
    if (dot == std::string::npos) {
        return 0;
    }
    return static_cast<int>(tickText.size() - dot - 1);
}

} // namespace

std::optional<Price> priceToTickDecimal(double value, double tickSize, PriceRounding rounding) {
    if (value <= 0.0 || !std::isfinite(value)) {
        return std::nullopt;
    }

    double normalized = value;
    int decimals = 16;
    if (tickSize > 0.0 && std::isfinite(tickSize)) {
        const double rawSteps = value / tickSize;
        const double epsilon = 1e-9;
        const double steps = rounding == PriceRounding::Down
            ? std::floor(rawSteps + epsilon)
            : std::ceil(rawSteps - epsilon);
        normalized = steps * tickSize;
        decimals = decimalsForTick(tickSize);
    }

    if (normalized <= 0.0 || !std::isfinite(normalized)) {
        return std::nullopt;
    }

    auto parsed = DecimalString::parse(fixedTrimmed(normalized, decimals));
    if (!parsed) {
        return std::nullopt;
    }
    return *parsed;
}

std::optional<Quantity> quantityToStepDecimal(double value, double stepSize) {
    if (value <= 0.0 || !std::isfinite(value)) {
        return std::nullopt;
    }

    double normalized = value;
    int decimals = 16;
    if (stepSize > 0.0 && std::isfinite(stepSize)) {
        const double rawSteps = value / stepSize;
        const double steps = std::floor(rawSteps + 1e-9);
        normalized = steps * stepSize;
        decimals = decimalsForTick(stepSize);
    }

    if (normalized <= 0.0 || !std::isfinite(normalized)) {
        return std::nullopt;
    }

    auto parsed = DecimalString::parse(fixedTrimmed(normalized, decimals));
    if (!parsed) {
        return std::nullopt;
    }
    return *parsed;
}

} // namespace engine
