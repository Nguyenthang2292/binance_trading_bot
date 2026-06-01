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

// WR-34: count the significant fractional digits of an exact decimal increment
// string (e.g. "0.00100000" -> 3). Returns -1 when the string is empty or does
// not represent a usable positive increment, signalling the caller to fall back
// to the lossy double-based count.
int decimalsFromIncrementString(std::string_view raw) {
    if (raw.empty()) {
        return -1;
    }
    const auto dot = raw.find('.');
    bool anyNonZeroDigit = false;
    for (const char c : raw) {
        if (c >= '1' && c <= '9') {
            anyNonZeroDigit = true;
            break;
        }
    }
    if (!anyNonZeroDigit) {
        return -1;  // "0", "0.0", ... — not a usable increment.
    }
    if (dot == std::string_view::npos) {
        return 0;
    }
    std::string_view frac = raw.substr(dot + 1);
    while (!frac.empty() && frac.back() == '0') {
        frac.remove_suffix(1);
    }
    return static_cast<int>(frac.size());
}

int resolveDecimals(double increment, std::string_view incrementRaw) {
    const int fromString = decimalsFromIncrementString(incrementRaw);
    return fromString >= 0 ? fromString : decimalsForTick(increment);
}

} // namespace

std::optional<Price> priceToTickDecimal(
    double value, double tickSize, std::string_view tickSizeRaw, PriceRounding rounding) {
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
        decimals = resolveDecimals(tickSize, tickSizeRaw);
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

std::optional<Price> priceToTickDecimal(double value, double tickSize, PriceRounding rounding) {
    return priceToTickDecimal(value, tickSize, std::string_view{}, rounding);
}

std::optional<Quantity> quantityToStepDecimal(double value, double stepSize, std::string_view stepSizeRaw) {
    if (value <= 0.0 || !std::isfinite(value)) {
        return std::nullopt;
    }

    double normalized = value;
    int decimals = 16;
    if (stepSize > 0.0 && std::isfinite(stepSize)) {
        const double rawSteps = value / stepSize;
        const double steps = std::floor(rawSteps + 1e-9);
        normalized = steps * stepSize;
        decimals = resolveDecimals(stepSize, stepSizeRaw);
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
    return quantityToStepDecimal(value, stepSize, std::string_view{});
}

} // namespace engine
