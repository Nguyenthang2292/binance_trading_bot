#include "transport/rate_limit_headers.h"

#include <charconv>
#include <cctype>
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

bool parseInt(std::string_view value, int& out) {
    int parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    out = parsed;
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
