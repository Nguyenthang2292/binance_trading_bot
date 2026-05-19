#pragma once

#include "types/account.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "orders/decimal_string.h"

namespace account {

enum class AccountTradeMode {
    Demo,
    Contest,
    Real,
    Unknown
};

enum class AccountCreditPolicy {
    ExplicitOnly,  // AccountCredit() returns error — Binance does not expose broker credit
    AssumeZero     // AccountCredit() returns 0.0 as MQL4 fallback; must be explicitly opted in
};

struct AccountCompatibilityConfig {
    std::string displayAsset{"USDT"};
    std::string company{"Binance"};
    std::string serverName;
    std::optional<std::string> accountName;
    std::optional<int64_t> loginOverride;
    AccountTradeMode tradeMode{AccountTradeMode::Unknown};
    bool expertTradeAllowed{true};
    AccountCreditPolicy creditPolicy{AccountCreditPolicy::ExplicitOnly};
};

enum class AccountSnapshotCompleteness {
    AccountOnly,
    AccountAndBalance,
    AccountAndPositions,
    AccountBalanceAndPositions,
    Full
};

struct AccountSnapshot {
    std::chrono::system_clock::time_point capturedAt;
    AccountSnapshotCompleteness completeness{AccountSnapshotCompleteness::AccountOnly};
    FuturesAccount account;
    std::optional<std::vector<Balance>> balances;
    std::optional<std::vector<Position>> positions;
    std::optional<bool> dualSidePosition;
    std::optional<bool> multiAssetsMargin;
    AccountCompatibilityConfig compatibility;
};

enum class AccountMappingError {
    Unsupported,          // no safe Binance semantic equivalent
    NotConfigured,        // required AccountCompatibilityConfig field not set
    SnapshotIncomplete,   // snapshot does not contain the data required for this property
    AmbiguousSymbol       // caller must supply a symbol (e.g. accountLeverage)
};

template <typename T>
using AccountMappingResult = std::expected<T, AccountMappingError>;

struct AccountSnapshotRequest {
    bool includeBalanceEndpoint{false};    // call /fapi/v2/balance in addition to account.assets
    bool includePositions{false};          // call /fapi/v2/positionRisk
    bool includeAccountConfig{false};      // call /fapi/v1/accountConfig for canTrade, dualSidePosition, multiAssetsMargin
    std::optional<std::string> positionFilter;  // scope positionRisk to single symbol when set
};

enum class MarginCheckSide {
    Buy,
    Sell
};

enum class MarginCheckCompleteness {
    ServerValidatedOnly,
    Estimated,
    Unavailable
};

struct MarginCheckDraft {
    std::string symbol;
    MarginCheckSide side;
    Quantity quantity;
    std::optional<Price> assumedPrice;
    bool useServerTestOrder{true};
};

struct MarginCheckResult {
    std::string symbol;
    MarginCheckSide side;
    std::optional<Quantity> quantity;
    MarginCheckCompleteness completeness{MarginCheckCompleteness::Unavailable};
    std::optional<std::string> estimatedRemainingFreeMargin;
    bool serverAccepted{false};
    std::optional<int> binanceCode;
    std::optional<std::string> binanceMessage;
};

enum class LiquidationRiskCompleteness {
    Unavailable,
    PositionOnly,
    BracketAware
};

struct LiquidationRiskView {
    std::optional<std::string> symbol;
    LiquidationRiskCompleteness completeness{LiquidationRiskCompleteness::Unavailable};
    std::vector<Position> positions;
    std::optional<std::string> note;
};

using AccountServiceError = std::variant<BinanceError, AccountMappingError>;

template <typename T>
using AccountServiceResult = std::expected<T, AccountServiceError>;

} // namespace account
