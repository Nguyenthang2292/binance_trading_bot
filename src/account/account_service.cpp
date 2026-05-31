/**
 * @file account_service.cpp
 * @brief AccountService implementation for snapshot, margin check, and risk view flows.
 */

#include "account/account_service.h"
#include "account/internal/string_utils.h"
#include "account/rest_account_client_adapter.h"

#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

namespace account {

namespace {

bool symbolEquals(const std::string& left, const std::string& right) {
    return internal::toUpper(left) == internal::toUpper(right);
}

std::string formatDecimal(double value, int precision = 8) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(static_cast<std::streamsize>(precision)) << value;
    auto text = out.str();
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.push_back('0');
    }
    return text.empty() ? "0.0" : text;
}

std::optional<double> parsePositiveFiniteDecimal(const DecimalString& value) {
    const double parsed = value.toDouble();
    if (!std::isfinite(parsed) || parsed <= 0.0) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<double> parseFiniteRawDecimal(std::string_view raw) {
    if (raw.empty()) {
        return std::nullopt;
    }
    auto parsed = DecimalString::parse(raw);
    if (!parsed) {
        return std::nullopt;
    }
    const double value = parsed->toDouble();
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

} // namespace

/**
 * @brief Construct an AccountService using an external REST client.
 *
 * The service will use the provided `IAccountRestClient` reference for
 * all network operations and will not take ownership of the client.
 *
 * @param rest Reference to an implementation of `IAccountRestClient` used
 *             to perform account-related REST calls.
 * @param compatibility Configuration affecting compatibility and mapping
 *                      behavior when producing snapshots.
 */
AccountService::AccountService(IAccountRestClient& rest, AccountCompatibilityConfig compatibility)
    : m_rest(rest), m_compatibility(std::move(compatibility)) {}

/**
 * @brief Construct an AccountService by wrapping a low-level RestClient.
 *
 * This constructor creates an owned adapter (`AccountRestClientAdapter`) that
 * adapts the provided `RestClient` to the `IAccountRestClient` interface. The
 * adapter is owned by the service instance.
 *
 * @param rest Reference to a low-level `RestClient` used to create an adapter.
 * @param compatibility Compatibility configuration for account mapping.
 */
AccountService::AccountService(RestClient& rest, AccountCompatibilityConfig compatibility)
    : m_ownedRest(std::make_unique<AccountRestClientAdapter>(rest)),
      m_rest(*m_ownedRest),
      m_compatibility(std::move(compatibility)) {}

/**
 * @brief Capture an account snapshot.
 *
 * The snapshot operation may include account metadata, balances, positions,
 * and account configuration depending on the `request` options. On error,
 * an `AccountServiceError` is returned via the `AccountServiceResult`.
 *
 * @param request Options that control inclusion of balances, positions and
 *                account configuration in the snapshot.
 * @return awaitable<AccountServiceResult<AccountSnapshot>> containing the
 *         populated `AccountSnapshot` or an `AccountServiceError` on failure.
 */
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
        snapshot.account.canTrade = snapshot.account.canTrade && config_res->canTrade;
        snapshot.dualSidePosition = config_res->dualSidePosition;
        snapshot.multiAssetsMargin = config_res->multiAssetsMargin;
        if (snapshot.completeness == AccountSnapshotCompleteness::AccountBalanceAndPositions) {
            snapshot.completeness = AccountSnapshotCompleteness::Full;
        }
    }

    co_return snapshot;
}

/**
 * @brief Check free margin for a prospective position.
 *
 * The method supports two complementary validation strategies:
 *  - `useServerTestOrder`: sends a test-order to the server and reports
 *    whether the server would accept the order.
 *  - `assumedPrice`: uses a locally-supplied assumed price and account
 *    balances to estimate remaining free margin after initial margin is
 *    reserved.
 *
 * @param draft A `MarginCheckDraft` describing the symbol, side, quantity
 *              and optional check behaviors.
 * @return awaitable<AccountServiceResult<MarginCheckResult>> with the
 *         estimated or server-validated margin check result, or an error.
 */
boost::asio::awaitable<AccountServiceResult<MarginCheckResult>> AccountService::checkFreeMargin(MarginCheckDraft draft) {
    if (draft.symbol.empty()) {
        co_return std::unexpected(AccountServiceError{AccountMappingError::SnapshotIncomplete});
    }
    if (!draft.useServerTestOrder && !draft.assumedPrice) {
        co_return std::unexpected(AccountServiceError{AccountMappingError::Unsupported});
    }

    MarginCheckResult result;
    result.symbol = draft.symbol;
    result.side = draft.side;
    result.quantity = std::optional<Quantity>{draft.quantity};
    result.completeness = MarginCheckCompleteness::Unavailable;

    if (draft.useServerTestOrder) {
        OrderRequest testReq;
        testReq.symbol = draft.symbol;
        testReq.side = draft.side == MarginCheckSide::Buy ? OrderSide::Buy : OrderSide::Sell;
        // checkFreeMargin currently validates only MARKET notional for server test-order.
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

    if (draft.assumedPrice.has_value()) {
        const auto parsedQuantity = parsePositiveFiniteDecimal(draft.quantity);
        const auto parsedPrice = parsePositiveFiniteDecimal(draft.assumedPrice.value());
        if (!parsedQuantity || !parsedPrice) {
            co_return result;
        }

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
            std::optional<double> availableBalance;
            if (acc_res->availableBalanceRaw.empty()) {
                availableBalance = acc_res->availableBalance;
            } else {
                availableBalance = parseFiniteRawDecimal(acc_res->availableBalanceRaw);
            }
            if (!availableBalance || !std::isfinite(*availableBalance)) {
                co_return result;
            }

            const double notional = *parsedQuantity * *parsedPrice;
            const double initialMargin = notional / static_cast<double>(leverage);
            const double remaining = *availableBalance - initialMargin;
            if (!std::isfinite(remaining)) {
                co_return result;
            }
            result.estimatedRemainingFreeMargin = formatDecimal(remaining);
            if (result.completeness == MarginCheckCompleteness::Unavailable) {
                result.completeness = MarginCheckCompleteness::Estimated;
            }
        }
    }

    co_return result;
}

/**
 * @brief Produce a liquidation risk view for an account or a specific symbol.
 *
 * Retrieves position rows from the REST client and composes a
 * `LiquidationRiskView` describing the current positions and a textual
 * note about completeness. This view does not include leverage bracket
 * or other instrument-specific risk data.
 *
 * @param symbol Optional symbol to scope the risk view to a single position.
 * @return awaitable<AccountServiceResult<LiquidationRiskView>> containing
 *         the assembled view or an `AccountServiceError` on failure.
 */
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
        if (view.symbol.has_value()) {
            view.note = "No position risk rows found for requested symbol";
        } else {
            view.note = "No position risk rows found for account scope";
        }
    } else {
        view.note = "Position-risk view only; leverage bracket data is not loaded";
    }
    co_return view;
}

} // namespace account
