#include "account/account_service.h"
#include "account/rest_account_client_adapter.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

namespace account {

namespace {

std::string toUpper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

bool symbolEquals(const std::string& left, const std::string& right) {
    return toUpper(left) == toUpper(right);
}

std::string formatDecimal(double value, int precision = 8) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    auto text = out.str();
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.push_back('0');
    }
    return text.empty() ? "0.0" : text;
}

} // namespace

AccountService::AccountService(IAccountRestClient& rest, AccountCompatibilityConfig compatibility)
    : m_rest(rest), m_compatibility(std::move(compatibility)) {}

AccountService::AccountService(RestClient& rest, AccountCompatibilityConfig compatibility)
    : m_ownedRest(std::make_unique<AccountRestClientAdapter>(rest)),
      m_rest(*m_ownedRest),
      m_compatibility(std::move(compatibility)) {}

boost::asio::awaitable<AccountServiceResult<AccountSnapshot>> AccountService::snapshot(AccountSnapshotRequest request) {
    auto acc_res = co_await m_rest.account();
    if (!acc_res) {
        co_return std::unexpected(AccountServiceError{acc_res.error()});
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
            co_return std::unexpected(AccountServiceError{bal_res.error()});
        }
    }

    const bool includePositions = request.includePositions || request.positionFilter.has_value();
    if (includePositions) {
        auto pos_res = co_await m_rest.positions(request.positionFilter);
        if (pos_res) {
            snapshot.positions = std::move(*pos_res);
            if (snapshot.completeness == AccountSnapshotCompleteness::AccountAndBalance) {
                snapshot.completeness = AccountSnapshotCompleteness::AccountBalanceAndPositions;
            } else {
                snapshot.completeness = AccountSnapshotCompleteness::AccountAndPositions;
            }
        } else {
            co_return std::unexpected(AccountServiceError{pos_res.error()});
        }
    }

    if (request.includeAccountConfig) {
        auto config_res = co_await m_rest.accountConfig();
        if (!config_res) {
            co_return std::unexpected(AccountServiceError{config_res.error()});
        }
        snapshot.account.canTrade = config_res->canTrade;
        snapshot.dualSidePosition = config_res->dualSidePosition;
        snapshot.multiAssetsMargin = config_res->multiAssetsMargin;
        snapshot.completeness = AccountSnapshotCompleteness::Full;
    }

    co_return snapshot;
}

boost::asio::awaitable<AccountServiceResult<MarginCheckResult>> AccountService::checkFreeMargin(MarginCheckDraft draft) {
    if (draft.symbol.empty()) {
        co_return std::unexpected(AccountServiceError{AccountMappingError::SnapshotIncomplete});
    }

    MarginCheckResult result{
        .symbol = draft.symbol,
        .side = draft.side,
        .quantity = draft.quantity,
        .completeness = MarginCheckCompleteness::Unavailable,
    };

    if (draft.useServerTestOrder) {
        OrderRequest testReq;
        testReq.symbol = draft.symbol;
        testReq.side = draft.side == MarginCheckSide::Buy ? OrderSide::Buy : OrderSide::Sell;
        testReq.type = OrderType::Market;
        testReq.quantity = std::string(draft.quantity.value());
        auto test_res = co_await m_rest.testOrder(std::move(testReq));
        if (!test_res) {
            if (test_res.error().category == ErrorCategory::Api) {
                result.completeness = MarginCheckCompleteness::ServerValidatedOnly;
                result.serverAccepted = false;
                result.binanceCode = test_res.error().code;
                result.binanceMessage = test_res.error().message;
            } else {
                co_return std::unexpected(AccountServiceError{test_res.error()});
            }
        } else {
            result.completeness = MarginCheckCompleteness::ServerValidatedOnly;
            result.serverAccepted = true;
        }
    }

    if (draft.assumedPrice) {
        auto acc_res = co_await m_rest.account();
        if (!acc_res) {
            co_return std::unexpected(AccountServiceError{acc_res.error()});
        }

        int leverage = 0;
        for (const auto& position : acc_res->positions) {
            if (symbolEquals(position.symbol, draft.symbol)) {
                leverage = position.leverage;
                break;
            }
        }

        if (leverage > 0) {
            const double notional = draft.quantity.toDouble() * draft.assumedPrice->toDouble();
            const double initialMargin = notional / static_cast<double>(leverage);
            const double remaining = acc_res->availableBalance - initialMargin;
            result.estimatedRemainingFreeMargin = formatDecimal(remaining);
            result.completeness = MarginCheckCompleteness::Estimated;
        }
    }

    co_return result;
}

boost::asio::awaitable<AccountServiceResult<LiquidationRiskView>> AccountService::liquidationRisk(std::optional<std::string> symbol) {
    auto pos_res = co_await m_rest.positions(symbol);
    if (!pos_res) {
        co_return std::unexpected(AccountServiceError{pos_res.error()});
    }

    LiquidationRiskView view;
    view.symbol = std::move(symbol);
    view.completeness = LiquidationRiskCompleteness::PositionOnly;
    view.positions = std::move(*pos_res);
    if (view.positions.empty()) {
        view.note = "No position risk rows found for requested scope";
    } else {
        view.note = "Position-risk view only; leverage bracket data is not loaded";
    }
    co_return view;
}

} // namespace account
