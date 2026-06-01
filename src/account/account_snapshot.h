#pragma once

/**
 * @file account_snapshot.h
 * @brief Shared account-domain types for snapshots, mappings, and service results.
 */

#include "common/expected_compat.h"
#include "types/account.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "orders/decimal_string.h"

namespace account {

/// Trade mode surfaced to compatibility consumers (for example MQL-style mapping).
enum class AccountTradeMode {
    Demo,
    Contest,
    Real,
    Unknown
};

/// Policy controlling how account credit is exposed when source data has no direct equivalent.
enum class AccountCreditPolicy {
    ExplicitOnly,  // AccountCredit() returns error — Binance does not expose broker credit
    AssumeZero     // AccountCredit() returns 0.0 as MQL4 fallback; must be explicitly opted in
};

/// Compatibility knobs used when mapping Binance account data to external models.
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

/// Data-completeness states for AccountSnapshot payload composition.
enum class AccountSnapshotCompleteness {
    AccountOnly,
    AccountAndBalance,
    AccountAndPositions,
    AccountBalanceAndPositions,
    Full
};

/// Immutable account capture with optional sections loaded on demand.
struct AccountSnapshot {
    std::chrono::system_clock::time_point capturedAt;
    AccountSnapshotCompleteness completeness{AccountSnapshotCompleteness::AccountOnly};
    FuturesAccount account;
    std::optional<std::vector<Balance>> balances;
    std::optional<std::vector<Position>> positions;
    std::optional<bool> dualSidePosition;
    std::optional<bool> multiAssetsMargin;
    AccountCompatibilityConfig compatibility;
    std::vector<BinanceError> partialErrors;
};

/// High-level mapping errors returned by adapter-style account APIs.
enum class AccountMappingError {
    Unsupported,          // no safe Binance semantic equivalent
    NotConfigured,        // required AccountCompatibilityConfig field not set
    SnapshotIncomplete,   // snapshot does not contain the data required for this property
    AmbiguousSymbol       // caller must supply a symbol (e.g. accountLeverage)
};

template <typename T>
using AccountMappingResult = compat::expected<T, AccountMappingError>;

struct AccountSnapshotRequest {
    bool includeBalanceEndpoint{false};    // call /fapi/v2/balance in addition to account.assets
    bool includePositions{false};          // call /fapi/v2/positionRisk
    bool includeAccountConfig{false};      // call /fapi/v1/accountConfig for canTrade, dualSidePosition, multiAssetsMargin
    bool allowPartialResults{false};        // keep successful sections when optional endpoint calls fail
    std::optional<std::string> positionFilter;  // scope positionRisk to single symbol when set
};

/// Direction for free-margin checks.
enum class MarginCheckSide {
    Buy,
    Sell
};

/// Confidence/completeness state for free-margin checks.
enum class MarginCheckCompleteness {
    ServerValidatedOnly,
    Estimated,
    Unavailable
};

/// Inputs required to evaluate whether a prospective order has enough free margin.
struct MarginCheckDraft {
    std::string symbol;
    MarginCheckSide side;
    PositionSide positionSide{PositionSide::Both};
    Quantity quantity;
    std::optional<Price> assumedPrice;
    std::optional<bool> reduceOnly;
    bool useServerTestOrder{true};
};

/// Output payload of free-margin evaluation.
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

/// Completeness of liquidation-risk information.
enum class LiquidationRiskCompleteness {
    Unavailable,
    PositionOnly,
    BracketAware
};

/// Position-scoped liquidation-risk view produced by AccountService.
struct LiquidationRiskView {
    std::optional<std::string> symbol;
    LiquidationRiskCompleteness completeness{LiquidationRiskCompleteness::Unavailable};
    std::vector<Position> positions;
    std::optional<std::string> note;
};

using AccountServiceError = std::variant<BinanceError, AccountMappingError>;

template <typename T>
using AccountServiceResult = compat::expected<T, AccountServiceError>;

} // namespace account
