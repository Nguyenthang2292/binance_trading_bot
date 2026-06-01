#pragma once

#include "orders/decimal_string.h"

#include <optional>
#include <string_view>

namespace engine {

enum class PriceRounding {
    Down,
    Up,
};

std::optional<Price> priceToTickDecimal(double value, double tickSize, PriceRounding rounding);
std::optional<Quantity> quantityToStepDecimal(double value, double stepSize);

// WR-34: overloads that take the exact tick/step decimal string (as sent by the
// exchange) so the formatted-decimal count is derived from the true increment
// rather than from a lossy double. Pass an empty/"0" raw to fall back to the
// double-based decimal count. The step arithmetic still uses the double.
std::optional<Price> priceToTickDecimal(
    double value, double tickSize, std::string_view tickSizeRaw, PriceRounding rounding);
std::optional<Quantity> quantityToStepDecimal(double value, double stepSize, std::string_view stepSizeRaw);

} // namespace engine
