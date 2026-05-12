#pragma once

#include "common/expected_compat.h"
#include "types/error.h"

#include <optional>
#include <string>
#include <string_view>

class DecimalString {
public:
    static std::expected<DecimalString, BinanceError> parse(std::string_view value);

    std::string_view value() const { return m_value; }
    double toDouble() const;

private:
    explicit DecimalString(std::string value) : m_value(std::move(value)) {}

    std::string m_value;
};

using Price = DecimalString;
using Quantity = DecimalString;
using TriggerPrice = DecimalString;
