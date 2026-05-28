#pragma once

/**
 * @file string_utils.h
 * @brief Internal string helpers shared by account-module implementations.
 */

#include <algorithm>
#include <cctype>
#include <string>

namespace account::internal {

/// Return an upper-cased copy of the input string using ASCII character rules.
inline std::string toUpper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

} // namespace account::internal
