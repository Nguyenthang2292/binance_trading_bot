#pragma once

/**
 * @file rest_account_client_adapter.h
 * @brief Adapter from RestClient to the IAccountRestClient interface.
 */

#include "account/irest_account_client.h"
#include "rest/rest_client.h"

namespace account {

/**
 * @brief Thin adapter that forwards account methods to `RestClient`.
 *
 * This keeps AccountService dependent on a small interface while preserving
 * the existing low-level REST implementation.
 */
class AccountRestClientAdapter final : public IAccountRestClient {
public:
    explicit AccountRestClientAdapter(RestClient& client) : m_client(client) {}

    boost::asio::awaitable<AccountRestResult<FuturesAccount>> account() override;
    boost::asio::awaitable<AccountRestResult<std::vector<Balance>>> balance() override;
    boost::asio::awaitable<AccountRestResult<std::vector<Position>>> positions(
        std::optional<std::string> symbol = {}) override;
    boost::asio::awaitable<AccountRestResult<FuturesAccountConfig>> accountConfig() override;
    boost::asio::awaitable<AccountRestResult<void>> testOrder(OrderRequest req) override;

private:
    RestClient& m_client;
};

} // namespace account
