#include "account/mql4_account_adapter.h"
#include "account/internal/string_utils.h"

#include <boost/config.hpp>

#include <cmath>
#include <utility>

/**
 * @file mql4_account_adapter.cpp
 * @brief Adapter implementation that maps AccountSnapshot data to MQL4-style
 *        account queries and properties.
 */

namespace account::mql4 {

namespace {
/**
 * @brief Small epsilon used when comparing margin values to zero.
 *
 * This avoids divide-by-near-zero instability when computing margin
 * percentages.
 */
constexpr double kMinMarginEpsilon = 1e-8;

/**
 * @brief Determine whether the snapshot indicates multi-assets margin mode.
 *
 * @param snapshot The account snapshot to inspect.
 * @return true if multi-assets margin is enabled; false otherwise.
 */
bool isMultiAssetsMarginEnabled(const AccountSnapshot& snapshot) {
    return snapshot.multiAssetsMargin.has_value() && *snapshot.multiAssetsMargin;
}

/**
 * @brief Ensure the provided snapshot is using single-asset margin mode.
 *
 * Some MQL4 mappings assume a single account asset; when the platform uses
 * multi-assets margin this adapter cannot provide a meaningful mapping.
 *
 * @param snapshot The account snapshot to validate.
 * @return Empty success result on single-asset mode, or an unexpected
 *         AccountMappingError::Unsupported when multi-assets margin is set.
 */
AccountMappingResult<void> requireSingleAssetMode(const AccountSnapshot& snapshot) {
    if (isMultiAssetsMarginEnabled(snapshot)) {
        return std::unexpected(AccountMappingError::Unsupported);
    }
    return {};
}

/**
 * @brief Locate the Balance entry corresponding to the configured display
 *        asset inside the provided snapshot.
 *
 * This function prefers explicit per-snapshot balances when present and
 * otherwise searches the account-level asset list. Returned pointers are
 * borrowed from snapshot-owned storage; callers MUST NOT outlive the
 * snapshot.
 *
 * @param snapshot The account snapshot to search.
 * @return Pointer to the requested Balance on success or an unexpected
 *         AccountMappingError when configuration is missing or snapshot is
 *         incomplete.
 */
[[nodiscard]] AccountMappingResult<const Balance*> displayAssetBalance(const AccountSnapshot& snapshot) {
    if (auto mode = requireSingleAssetMode(snapshot); !mode) {
        return std::unexpected(mode.error());
    }

    if (snapshot.compatibility.displayAsset.empty()) {
        return std::unexpected(AccountMappingError::NotConfigured);
    }

    const auto wantedAsset = internal::toUpper(snapshot.compatibility.displayAsset);
    if (snapshot.balances) {
        for (const auto& balance : *snapshot.balances) {
            if (internal::toUpper(balance.asset) == wantedAsset) {
                // Borrowed pointer into snapshot-owned storage.
                return &balance;
            }
        }
    }

    for (const auto& balance : snapshot.account.assets) {
        if (internal::toUpper(balance.asset) == wantedAsset) {
            // Borrowed pointer into snapshot-owned storage.
            return &balance;
        }
    }

    return std::unexpected(AccountMappingError::SnapshotIncomplete);
}

/**
 * @brief Case-insensitive comparison for trading symbols.
 *
 * @param left Left-hand symbol string.
 * @param right Right-hand symbol string.
 * @return true when symbols match ignoring ASCII case; false otherwise.
 */
bool matchesSymbolInsensitive(const std::string& left, const std::string& right) {
    return internal::toUpper(left) == internal::toUpper(right);
}

} // namespace

Mql4AccountAdapter::Mql4AccountAdapter(AccountSnapshot snapshot)
    : m_snapshot(std::move(snapshot)) {}

AccountMappingResult<double> Mql4AccountAdapter::accountInfoDouble(AccountDoubleProperty property) const {
    switch (property) {
        case AccountDoubleProperty::Balance:
            return accountBalance();
        case AccountDoubleProperty::Credit:
            return accountCredit();
        case AccountDoubleProperty::Profit:
            return accountProfit();
        case AccountDoubleProperty::Equity:
            return accountEquity();
        case AccountDoubleProperty::Margin:
            return accountMargin();
        case AccountDoubleProperty::FreeMargin:
            return accountFreeMargin();
        case AccountDoubleProperty::MarginLevel: {
            auto margin = accountMargin();
            if (!margin) {
                return std::unexpected(margin.error());
            }
            if (std::abs(*margin) < kMinMarginEpsilon) {
                return 0.0;
            }
            auto equity = accountEquity();
            if (!equity) {
                return std::unexpected(equity.error());
            }
            return (*equity / *margin) * 100.0;
        }
        case AccountDoubleProperty::MarginStopoutCall:
        case AccountDoubleProperty::MarginStopoutStop:
            return std::unexpected(AccountMappingError::Unsupported);
    }
    return std::unexpected(AccountMappingError::Unsupported);
}

AccountMappingResult<int64_t> Mql4AccountAdapter::accountInfoInteger(AccountIntegerProperty property) const {
    switch (property) {
        case AccountIntegerProperty::Login:
            return accountNumber();
        case AccountIntegerProperty::TradeMode:
            return static_cast<int64_t>(m_snapshot.compatibility.tradeMode);
        case AccountIntegerProperty::Leverage:
            // Symbol-scoped on Binance Futures; caller must use accountLeverage(symbol).
            return std::unexpected(AccountMappingError::AmbiguousSymbol);
        case AccountIntegerProperty::LimitOrders:
            return std::unexpected(AccountMappingError::Unsupported);
        case AccountIntegerProperty::MarginStopoutMode:
            return std::unexpected(AccountMappingError::Unsupported);
        case AccountIntegerProperty::TradeAllowed:
            return m_snapshot.account.canTrade ? 1 : 0;
        case AccountIntegerProperty::TradeExpert:
            return (m_snapshot.account.canTrade && m_snapshot.compatibility.expertTradeAllowed) ? 1 : 0;
    }
    return std::unexpected(AccountMappingError::Unsupported);
}

AccountMappingResult<std::string> Mql4AccountAdapter::accountInfoString(AccountStringProperty property) const {
    switch (property) {
        case AccountStringProperty::Name:
            return accountName();
        case AccountStringProperty::Server:
            return accountServer();
        case AccountStringProperty::Currency:
            return accountCurrency();
        case AccountStringProperty::Company:
            return accountCompany();
    }
    return std::unexpected(AccountMappingError::Unsupported);
}

AccountMappingResult<double> Mql4AccountAdapter::accountBalance() const {
    const auto balance = displayAssetBalance(m_snapshot);
    if (!balance) {
        return std::unexpected(balance.error());
    }
    return (*balance)->walletBalance;
}

AccountMappingResult<double> Mql4AccountAdapter::accountCredit() const {
    if (m_snapshot.compatibility.creditPolicy == AccountCreditPolicy::AssumeZero) {
        return 0.0;
    }
    return std::unexpected(AccountMappingError::Unsupported);
}

AccountMappingResult<std::string> Mql4AccountAdapter::accountCompany() const {
    if (m_snapshot.compatibility.company.empty()) {
        return std::unexpected(AccountMappingError::NotConfigured);
    }
    return m_snapshot.compatibility.company;
}

AccountMappingResult<std::string> Mql4AccountAdapter::accountCurrency() const {
    if (m_snapshot.compatibility.displayAsset.empty()) {
        return std::unexpected(AccountMappingError::NotConfigured);
    }
    return m_snapshot.compatibility.displayAsset;
}

AccountMappingResult<double> Mql4AccountAdapter::accountEquity() const {
    const auto balance = displayAssetBalance(m_snapshot);
    if (!balance) {
        return std::unexpected(balance.error());
    }
    return (*balance)->marginBalance;
}

AccountMappingResult<double> Mql4AccountAdapter::accountFreeMargin() const {
    if (auto mode = requireSingleAssetMode(m_snapshot); !mode) {
        return std::unexpected(mode.error());
    }

    const auto balance = displayAssetBalance(m_snapshot);
    if (!balance) {
        return std::unexpected(balance.error());
    }

    // Keep source aligned with accountMargin() so Equity - Margin equals FreeMargin.
    return (*balance)->marginBalance - (*balance)->initialMargin;
}

AccountMappingResult<int64_t> Mql4AccountAdapter::accountLeverage(std::string symbol) const {
    if (symbol.empty()) {
        return std::unexpected(AccountMappingError::AmbiguousSymbol);
    }

    const auto requested = internal::toUpper(std::move(symbol));

    if (m_snapshot.positions) {
        for (const auto& position : *m_snapshot.positions) {
            if (matchesSymbolInsensitive(position.symbol, requested)) {
                return position.leverage;
            }
        }
    }
    for (const auto& position : m_snapshot.account.positions) {
        if (matchesSymbolInsensitive(position.symbol, requested)) {
            return position.leverage;
        }
    }
    return std::unexpected(AccountMappingError::SnapshotIncomplete);
}

AccountMappingResult<double> Mql4AccountAdapter::accountMargin() const {
    if (auto mode = requireSingleAssetMode(m_snapshot); !mode) {
        return std::unexpected(mode.error());
    }

    const auto balance = displayAssetBalance(m_snapshot);
    if (!balance) {
        return std::unexpected(balance.error());
    }

    return (*balance)->initialMargin;
}

AccountMappingResult<std::string> Mql4AccountAdapter::accountName() const {
    if (m_snapshot.compatibility.accountName) {
        return *m_snapshot.compatibility.accountName;
    }
    return std::unexpected(AccountMappingError::NotConfigured);
}

AccountMappingResult<int64_t> Mql4AccountAdapter::accountNumber() const {
    if (m_snapshot.compatibility.loginOverride) {
        return *m_snapshot.compatibility.loginOverride;
    }
    return std::unexpected(AccountMappingError::NotConfigured);
}

AccountMappingResult<double> Mql4AccountAdapter::accountProfit() const {
    const auto balance = displayAssetBalance(m_snapshot);
    if (!balance) {
        return std::unexpected(balance.error());
    }
    return (*balance)->unrealizedProfit;
}

AccountMappingResult<std::string> Mql4AccountAdapter::accountServer() const {
    if (m_snapshot.compatibility.serverName.empty()) {
        return std::unexpected(AccountMappingError::NotConfigured);
    }
    return m_snapshot.compatibility.serverName;
}

std::chrono::system_clock::time_point Mql4AccountAdapter::capturedAt() const {
    return m_snapshot.capturedAt;
}

} // namespace account::mql4
