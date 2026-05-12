#pragma once

#include <boost/system/error_code.hpp>
#include <string>
#include <string_view>

enum class ErrorCategory {
    Network,
    Api,
    RateLimit,
    Auth,
    Parse,
};

struct BinanceError {
    ErrorCategory category{ErrorCategory::Api};
    int code{0};
    std::string message;

    std::string toString() const;

    static BinanceError fromApiResponse(int code, std::string_view msg);
    static BinanceError fromNetwork(boost::system::error_code ec);
    static BinanceError fromHttp(int httpStatus, std::string_view body);
    static BinanceError fromParse(std::string_view detail);
};
