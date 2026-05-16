#pragma once

#include "orders/decimal_string.h"

#include <optional>

namespace engine {

enum class PriceRounding {
    Down,
    Up,
};

std::optional<Price> priceToTickDecimal(double value, double tickSize, PriceRounding rounding);
std::optional<Quantity> quantityToStepDecimal(double value, double stepSize);

} // namespace engine
