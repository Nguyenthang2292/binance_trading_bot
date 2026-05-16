#pragma once

#include "account/irest_account_client.h"
#include "account/account_snapshot.h"

#include <boost/asio/awaitable.hpp>

#include <memory>

class RestClient;

namespace account {

class AccountService {
public:
    AccountService(IAccountRestClient& rest, AccountCompatibilityConfig compatibility);
    AccountService(RestClient& rest, AccountCompatibilityConfig compatibility);
    AccountService(const AccountService&) = delete;
    AccountService& operator=(const AccountService&) = delete;
    AccountService(AccountService&&) = delete;
    AccountService& operator=(AccountService&&) = delete;

    boost::asio::awaitable<AccountServiceResult<AccountSnapshot>> snapshot(AccountSnapshotRequest request = {});
    boost::asio::awaitable<AccountServiceResult<MarginCheckResult>> checkFreeMargin(MarginCheckDraft draft);
    boost::asio::awaitable<AccountServiceResult<LiquidationRiskView>> liquidationRisk(std::optional<std::string> symbol = std::nullopt);

private:
    std::unique_ptr<IAccountRestClient> m_ownedRest;
    IAccountRestClient& m_rest;
    AccountCompatibilityConfig m_compatibility;
};

} // namespace account
