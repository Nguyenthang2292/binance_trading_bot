#include "types/error.h"

#include <boost/asio/error.hpp>
#include <simdjson.h>

#include <algorithm>
#include <sstream>
#include <string>

namespace {

std::string categoryName(ErrorCategory category) {
    switch (category) {
        case ErrorCategory::Network: return "Network";
        case ErrorCategory::Api: return "Api";
        case ErrorCategory::RateLimit: return "RateLimit";
        case ErrorCategory::Auth: return "Auth";
        case ErrorCategory::Parse: return "Parse";
    }
    return "Unknown";
}

std::string sanitizeRawHttpBody(std::string_view body) {
    constexpr size_t kMaxBodyMessage = 1024;
    std::string out;
    out.reserve(std::min(body.size(), kMaxBodyMessage) + 16);
    for (unsigned char ch : body) {
        if (out.size() >= kMaxBodyMessage) {
            out += "...[truncated]";
            break;
        }
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            out.push_back(' ');
            continue;
        }
        if (ch < 0x20 || ch == 0x7f) {
            out.push_back('?');
            continue;
        }
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

} // namespace

std::string BinanceError::toString() const {
    std::ostringstream out;
    out << categoryName(category) << " [" << code << "]: " << message;
    return out.str();
}

bool BinanceError::isOperationAbortedBeforeSend() const {
    return category == ErrorCategory::Network
        && networkPhase == NetworkErrorPhase::BeforeSend
        && systemError.has_value()
        && *systemError == boost::asio::error::operation_aborted;
}

BinanceError BinanceError::fromApiResponse(int code, std::string_view msg) {
    ErrorCategory category = ErrorCategory::Api;
    if (code == -2014 || code == -2015 || code == -1022) {
        category = ErrorCategory::Auth;
    }
    return {category, code, std::string(msg)};
}

BinanceError BinanceError::fromNetwork(boost::system::error_code ec, NetworkErrorPhase phase) {
    return {
        .category = ErrorCategory::Network,
        .code = ec.value(),
        .message = ec.message(),
        .systemError = ec,
        .networkPhase = phase,
    };
}

BinanceError BinanceError::fromHttp(int httpStatus, std::string_view body) {
    if (httpStatus == 429 || httpStatus == 418) {
        return {ErrorCategory::RateLimit, httpStatus, sanitizeRawHttpBody(body)};
    }

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto parseError = parser.parse(body).get(doc);
    if (!parseError && doc.is_object()) {
        int64_t code = httpStatus;
        std::string_view msg = body;
        (void)doc["code"].get(code);
        (void)doc["msg"].get(msg);
        return fromApiResponse(static_cast<int>(code), msg);
    }

    if (httpStatus == 401 || httpStatus == 403) {
        return {ErrorCategory::Auth, httpStatus, sanitizeRawHttpBody(body)};
    }

    return {ErrorCategory::Api, httpStatus, sanitizeRawHttpBody(body)};
}

BinanceError BinanceError::fromParse(std::string_view detail) {
    return {ErrorCategory::Parse, 0, std::string(detail)};
}
