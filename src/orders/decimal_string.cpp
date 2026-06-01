#include "orders/decimal_string.h"

#include <cctype>
#include <stdexcept>

namespace {

BinanceError invalidDecimal(std::string_view value) {
    return BinanceError::fromApiResponse(-90001, std::string("Invalid decimal value: ") + std::string(value));
}

} // namespace

compat::expected<DecimalString, BinanceError> DecimalString::parse(std::string_view value) {
    if (value.empty()) {
        return compat::unexpected(invalidDecimal(value));
    }

    bool hasDigit = false;
    bool hasDot = false;
    for (char c : value) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isdigit(uc)) {
            hasDigit = true;
            continue;
        }
        if (c == '.') {
            if (hasDot) {
                return compat::unexpected(invalidDecimal(value));
            }
            hasDot = true;
            continue;
        }
        return compat::unexpected(invalidDecimal(value));
    }

    if (!hasDigit) {
        return compat::unexpected(invalidDecimal(value));
    }

    return DecimalString(std::string(value));
}

double DecimalString::toDouble() const {
    try {
        return std::stod(m_value);
    } catch (const std::exception&) {
        return 0.0;
    }
}
