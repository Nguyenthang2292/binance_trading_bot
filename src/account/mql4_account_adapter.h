#pragma once

/**
 * @file mql4_account_adapter.h
 * @brief MQL4-style account property mapping over AccountSnapshot data.
 */

#include "account/account_snapshot.h"
#include "types/error.h"

#include <string>

namespace account::mql4 {

/// MQL4-compatible account double properties.
enum class AccountDoubleProperty {
    Balance,
    Credit,
    Profit,
    Equity,
    Margin,
    FreeMargin,
    MarginLevel,
    MarginStopoutCall,
    MarginStopoutStop
};

/// MQL4-compatible account integer properties.
enum class AccountIntegerProperty {
    Login,
    TradeMode,
    Leverage,
    LimitOrders,
    MarginStopoutMode,
    TradeAllowed,
    TradeExpert
};

/// MQL4-compatible account string properties.
enum class AccountStringProperty {
    Name,
    Server,
    Currency,
    Company
};

/**
 * @brief Translate AccountSnapshot values to the MQL4 account property model.
 *
 * Methods return `AccountMappingResult<T>` to preserve explicit error states
 * when a value is unsupported, ambiguous, or missing from the snapshot.
 */
class Mql4AccountAdapter {
public:
    explicit Mql4AccountAdapter(::account::AccountSnapshot snapshot);

    ::account::AccountMappingResult<double> accountInfoDouble(AccountDoubleProperty property) const;
    ::account::AccountMappingResult<int64_t> accountInfoInteger(AccountIntegerProperty property) const;
    ::account::AccountMappingResult<std::string> accountInfoString(AccountStringProperty property) const;

    ::account::AccountMappingResult<double> accountBalance() const;
    ::account::AccountMappingResult<double> accountCredit() const;
    ::account::AccountMappingResult<std::string> accountCompany() const;
    ::account::AccountMappingResult<std::string> accountCurrency() const;
    ::account::AccountMappingResult<double> accountEquity() const;
    ::account::AccountMappingResult<double> accountFreeMargin() const;
    ::account::AccountMappingResult<int64_t> accountLeverage(std::string symbol) const;
    ::account::AccountMappingResult<double> accountMargin() const;
    ::account::AccountMappingResult<std::string> accountName() const;
    ::account::AccountMappingResult<int64_t> accountNumber() const;
    ::account::AccountMappingResult<double> accountProfit() const;
    ::account::AccountMappingResult<std::string> accountServer() const;
    std::chrono::system_clock::time_point capturedAt() const;

private:
    ::account::AccountSnapshot m_snapshot;
};

} // namespace account::mql4
