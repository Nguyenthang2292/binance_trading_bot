#include "account/account_service.h"
#include "account/rest_account_client_adapter.h"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/system/error_code.hpp>

#include <memory>
#include <utility>

namespace account {

AccountService::AccountService(IAccountRestClient& rest, AccountCompatibilityConfig compatibility)
    : m_rest(rest), m_compatibility(std::move(compatibility)) {}

AccountService::AccountService(RestClient& rest, AccountCompatibilityConfig compatibility)
    : m_ownedRest(std::make_unique<AccountRestClientAdapter>(rest)),
      m_rest(*m_ownedRest),
      m_compatibility(std::move(compatibility)) {}

boost::asio::awaitable<AccountServiceResult<AccountSnapshot>> AccountService::snapshot(AccountSnapshotRequest request) {
    if (request.includeAccountConfig) {
        co_return std::unexpected(AccountMappingError::Unsupported);
    }

    auto acc_res = co_await m_rest.account();
    if (!acc_res) {
        co_return std::unexpected(acc_res.error());
    }

    AccountSnapshot snapshot;
    snapshot.capturedAt = std::chrono::system_clock::now();
    snapshot.completeness = AccountSnapshotCompleteness::AccountOnly;
    snapshot.account = std::move(*acc_res);
    snapshot.compatibility = m_compatibility;

    if (request.includeBalanceEndpoint) {
        auto bal_res = co_await m_rest.balance();
        if (bal_res) {
            snapshot.balances = std::move(*bal_res);
            snapshot.completeness = AccountSnapshotCompleteness::AccountAndBalance;
        } else {
            co_return std::unexpected(bal_res.error());
        }
    }

    if (request.includePositions) {
        auto pos_res = co_await m_rest.positions(request.positionFilter);
        if (pos_res) {
            snapshot.positions = std::move(*pos_res);
            if (snapshot.completeness == AccountSnapshotCompleteness::AccountAndBalance) {
                snapshot.completeness = AccountSnapshotCompleteness::AccountBalanceAndPositions;
            } else {
                snapshot.completeness = AccountSnapshotCompleteness::AccountAndPositions;
            }
        } else {
            co_return std::unexpected(pos_res.error());
        }
    }

    co_return snapshot;
}

boost::asio::awaitable<AccountServiceResult<MarginCheckResult>> AccountService::checkFreeMargin(MarginCheckDraft draft) {
    // Phase C stub: return success with Unavailable completeness
    MarginCheckResult result;
    result.symbol = std::move(draft.symbol);
    result.side = draft.side;
    result.quantity = draft.quantity;
    result.completeness = MarginCheckCompleteness::Unavailable;
    co_return result;
}

boost::asio::awaitable<AccountServiceResult<LiquidationRiskView>> AccountService::liquidationRisk(std::optional<std::string> symbol) {
    // Phase C stub: return Unsupported mapping error
    co_return std::unexpected(AccountMappingError::Unsupported);
}

} // namespace account
