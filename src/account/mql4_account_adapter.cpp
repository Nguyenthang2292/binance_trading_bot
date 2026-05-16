#include "account/mql4_account_adapter.h"

#include <boost/config.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace account::mql4 {

namespace {
constexpr double kMinMarginEpsilon = 1e-8;

std::string toUpper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

bool isMultiAssetsMarginEnabled(const AccountSnapshot& snapshot) {
    return snapshot.multiAssetsMargin.has_value() && *snapshot.multiAssetsMargin;
}

AccountMappingResult<void> requireSingleAssetMode(const AccountSnapshot& snapshot) {
    if (isMultiAssetsMarginEnabled(snapshot)) {
        return std::unexpected(AccountMappingError::Unsupported);
    }
    return {};
}

AccountMappingResult<const Balance*> displayAssetBalance(const AccountSnapshot& snapshot) {
    if (auto mode = requireSingleAssetMode(snapshot); !mode) {
        return std::unexpected(mode.error());
    }

    if (snapshot.compatibility.displayAsset.empty()) {
        return std::unexpected(AccountMappingError::NotConfigured);
    }

    const auto wantedAsset = toUpper(snapshot.compatibility.displayAsset);
    if (snapshot.balances) {
        for (const auto& balance : *snapshot.balances) {
            if (toUpper(balance.asset) == wantedAsset) {
                return &balance;
            }
        }
    }

    for (const auto& balance : snapshot.account.assets) {
        if (toUpper(balance.asset) == wantedAsset) {
            return &balance;
        }
    }

    return std::unexpected(AccountMappingError::SnapshotIncomplete);
}

const std::vector<Position>& positionsForMapping(const AccountSnapshot& snapshot) {
    if (snapshot.positions) {
        return *snapshot.positions;
    }
    return snapshot.account.positions;
}

double positionsInitialMargin(const std::vector<Position>& positions) {
    double total = 0.0;
    for (const auto& position : positions) {
        total += position.initialMargin;
    }
    return total;
}

bool matchesSymbolInsensitive(const std::string& left, const std::string& right) {
    return toUpper(left) == toUpper(right);
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

    // MQL4 FreeMargin maps to Equity - Margin (positions only).
    // Binance availableBalance subtracts open-order initial margin, so avoid using it here.
    return (*balance)->marginBalance - (*balance)->initialMargin;
}

AccountMappingResult<int64_t> Mql4AccountAdapter::accountLeverage(std::string symbol) const {
    if (symbol.empty()) {
        return std::unexpected(AccountMappingError::AmbiguousSymbol);
    }

    const auto requested = toUpper(std::move(symbol));

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

    const auto& positions = positionsForMapping(m_snapshot);
    if (!positions.empty()) {
        return positionsInitialMargin(positions);
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

} // namespace account::mql4
