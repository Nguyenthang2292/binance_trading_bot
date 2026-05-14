#pragma once

#include "common/expected_compat.h"
#include "types/account.h"
#include "types/error.h"

#include <boost/asio/awaitable.hpp>

#include <optional>
#include <string>
#include <vector>

template <typename T>
using AccountRestResult = std::expected<T, BinanceError>;

class IAccountRestClient {
public:
    virtual ~IAccountRestClient() = default;

    virtual boost::asio::awaitable<AccountRestResult<FuturesAccount>> account() = 0;
    virtual boost::asio::awaitable<AccountRestResult<std::vector<Balance>>> balance() = 0;
    virtual boost::asio::awaitable<AccountRestResult<std::vector<Position>>> positions(
        std::optional<std::string> symbol = {}) = 0;
};

