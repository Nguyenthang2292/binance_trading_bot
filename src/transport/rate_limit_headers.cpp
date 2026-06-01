#include "transport/rate_limit_headers.h"

#include <charconv>
#include <cctype>
#include <limits>
#include <system_error>

namespace {

bool equalsHeaderName(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        const auto left = static_cast<unsigned char>(lhs[i]);
        const auto right = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

std::string_view trimAsciiWhitespace(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

bool parseInt(std::string_view value, int& out) {
    value = trimAsciiWhitespace(value);
    long long parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec == std::errc::result_out_of_range && result.ptr == end) {
        out = value.starts_with("-") ? 0 : std::numeric_limits<int>::max();
        return true;
    }
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    if (parsed < 0) {
        out = 0;
        return true;
    }
    if (parsed > std::numeric_limits<int>::max()) {
        out = std::numeric_limits<int>::max();
        return true;
    }
    out = static_cast<int>(parsed);
    return true;
}

} // namespace

void applyRateLimitHeader(RateLimitHeaderUsage& usage,
                          std::string_view name,
                          std::string_view value) {
    int parsed = 0;
    if (!parseInt(value, parsed)) {
        return;
    }

    if (equalsHeaderName(name, "X-MBX-USED-WEIGHT-1M")) {
        usage.usedWeight1m = parsed;
        return;
    }
    if (equalsHeaderName(name, "X-MBX-ORDER-COUNT-1M")) {
        usage.usedOrders1m = parsed;
        return;
    }
    if (equalsHeaderName(name, "X-MBX-ORDER-COUNT-10S")) {
        usage.usedOrders10s = parsed;
    }
}
