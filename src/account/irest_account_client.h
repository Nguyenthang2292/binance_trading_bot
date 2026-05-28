#pragma once

/**
 * @file irest_account_client.h
 * @brief Interface abstraction for account-focused REST operations.
 */

#include "common/expected_compat.h"
#include "types/trade.h"
#include "types/account.h"
#include "types/error.h"

#include <boost/asio/awaitable.hpp>

#include <optional>
#include <string>
#include <vector>

namespace account {

template <typename T>
using AccountRestResult = std::expected<T, BinanceError>;

/**
 * @brief Async contract for fetching account data and validating test orders.
 *
 * Implementations map concrete transport clients into this minimal interface so
 * higher layers can be tested without direct network dependencies.
 */
class IAccountRestClient {
public:
    virtual ~IAccountRestClient() = default;

    virtual boost::asio::awaitable<AccountRestResult<FuturesAccount>> account() = 0;
    virtual boost::asio::awaitable<AccountRestResult<std::vector<Balance>>> balance() = 0;
    virtual boost::asio::awaitable<AccountRestResult<std::vector<Position>>> positions(
        std::optional<std::string> symbol = {}) = 0;
    virtual boost::asio::awaitable<AccountRestResult<FuturesAccountConfig>> accountConfig() = 0;
    virtual boost::asio::awaitable<AccountRestResult<void>> testOrder(OrderRequest req) = 0;
};

} // namespace account
