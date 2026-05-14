#pragma once

#include "account/irest_account_client.h"
#include "rest/rest_client.h"

class AccountRestClientAdapter final : public IAccountRestClient {
public:
    explicit AccountRestClientAdapter(RestClient& client) : m_client(client) {}

    boost::asio::awaitable<AccountRestResult<FuturesAccount>> account() override;
    boost::asio::awaitable<AccountRestResult<std::vector<Balance>>> balance() override;
    boost::asio::awaitable<AccountRestResult<std::vector<Position>>> positions(
        std::optional<std::string> symbol = {}) override;

private:
    RestClient& m_client;
};

