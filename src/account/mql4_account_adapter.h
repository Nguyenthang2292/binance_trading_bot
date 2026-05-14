#pragma once

#include "account/account_snapshot.h"
#include "types/error.h"

#include <string>

namespace account::mql4 {

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

enum class AccountIntegerProperty {
    Login,
    TradeMode,
    Leverage,
    LimitOrders,
    MarginStopoutMode,
    TradeAllowed,
    TradeExpert
};

enum class AccountStringProperty {
    Name,
    Server,
    Currency,
    Company
};

class Mql4AccountAdapter {
public:
    explicit Mql4AccountAdapter(AccountSnapshot snapshot);

    AccountMappingResult<double> accountInfoDouble(AccountDoubleProperty property) const;
    AccountMappingResult<int64_t> accountInfoInteger(AccountIntegerProperty property) const;
    AccountMappingResult<std::string> accountInfoString(AccountStringProperty property) const;

    AccountMappingResult<double> accountBalance() const;
    AccountMappingResult<double> accountCredit() const;
    AccountMappingResult<std::string> accountCompany() const;
    AccountMappingResult<std::string> accountCurrency() const;
    AccountMappingResult<double> accountEquity() const;
    AccountMappingResult<double> accountFreeMargin() const;
    AccountMappingResult<int64_t> accountLeverage(std::string symbol) const;
    AccountMappingResult<double> accountMargin() const;
    AccountMappingResult<std::string> accountName() const;
    AccountMappingResult<int64_t> accountNumber() const;
    AccountMappingResult<double> accountProfit() const;
    AccountMappingResult<std::string> accountServer() const;

private:
    AccountSnapshot m_snapshot;
};

} // namespace account::mql4
