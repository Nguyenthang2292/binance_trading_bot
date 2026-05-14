#pragma once

#include <boost/config.hpp>
#include <boost/system/error_code.hpp>
#include <optional>
#include <string>
#include <string_view>

enum class ErrorCategory {
    Network,
    Api,
    RateLimit,
    Auth,
    Parse,
};

enum class NetworkErrorPhase {
    Unknown,
    BeforeSend,
    AfterSend,
};

struct BinanceError {
    ErrorCategory category{ErrorCategory::Api};
    int code{0};
    std::string message;
    std::optional<boost::system::error_code> systemError;
    NetworkErrorPhase networkPhase{NetworkErrorPhase::Unknown};

    std::string toString() const;
    bool isOperationAbortedBeforeSend() const;

    static BinanceError fromApiResponse(int code, std::string_view msg);
    static BinanceError fromNetwork(
        boost::system::error_code ec,
        NetworkErrorPhase phase = NetworkErrorPhase::Unknown);
    static BinanceError fromHttp(int httpStatus, std::string_view body);
    static BinanceError fromParse(std::string_view detail);
};
