#pragma once

/**
 * @file account_service.h
 * @brief Asynchronous service for collecting account snapshots and margin checks.
 */

#include "account/irest_account_client.h"
#include "account/account_snapshot.h"

#include <boost/asio/awaitable.hpp>

#include <memory>

class RestClient;

namespace account {

/**
 * @brief Orchestrates account-related REST calls into higher-level snapshots.
 *
 * The service can be constructed from either an injected account REST interface
 * or a low-level `RestClient` that is wrapped by an adapter.
 */
class AccountService {
public:
    /// Construct with an externally-managed account REST client.
    AccountService(IAccountRestClient& rest, AccountCompatibilityConfig compatibility);
    /// Construct with a low-level RestClient wrapped by an owned adapter.
    AccountService(RestClient& rest, AccountCompatibilityConfig compatibility);
    AccountService(const AccountService&) = delete;
    AccountService& operator=(const AccountService&) = delete;
    AccountService(AccountService&&) = delete;
    AccountService& operator=(AccountService&&) = delete;

    /// Fetch account data according to the request and return a typed snapshot result.
    boost::asio::awaitable<AccountServiceResult<AccountSnapshot>> snapshot(AccountSnapshotRequest request = {});
    /// Evaluate free-margin feasibility through server validation and/or local estimation.
    boost::asio::awaitable<AccountServiceResult<MarginCheckResult>> checkFreeMargin(MarginCheckDraft draft);
    /// Build a position-based liquidation-risk view for one symbol or the whole account.
    boost::asio::awaitable<AccountServiceResult<LiquidationRiskView>> liquidationRisk(std::optional<std::string> symbol = std::nullopt);

private:
    // Must be declared before m_rest so RestClient constructor can bind m_rest to *m_ownedRest.
    std::unique_ptr<IAccountRestClient> m_ownedRest;
    IAccountRestClient& m_rest;
    AccountCompatibilityConfig m_compatibility;
};

} // namespace account
