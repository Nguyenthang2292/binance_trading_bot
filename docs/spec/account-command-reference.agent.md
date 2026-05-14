---
doc_type: agent-reference
source: docs/spec/account-command-reference.md
generated: 2026-05-14
target_audience: AI coding agents
language: en
format_notes: >
  Structured for deterministic parsing. All types and signatures match the
  current C++ account implementation. Use "source_file" fields to navigate the
  codebase. "INVARIANT" lines are hard constraints. "PRECONDITION" lines must be
  true before calling. "POSTCONDITION" lines are guaranteed on success path.
---

# Account Layer - Agent Reference

## 0. Quick Navigation

| Task | Section |
|---|---|
| Find service signatures | Section 2 AccountService API |
| Understand snapshot flags | Section 3 Snapshot Semantics |
| Map MQL4 getters | Section 4 MQL4 Adapter |
| Understand types | Section 5 Type Catalog |
| Handle failures | Section 6 Error Rules |
| Find source file for a symbol | Section 8 File Index |

---

## 1. Architecture

```
AccountService                  <- async facade
|-- IAccountRestClient           <- narrow testable REST seam
`-- AccountRestClientAdapter     <- delegates to RestClient

account::mql4::Mql4AccountAdapter <- snapshot-only MQL4 compatibility wrapper
```

- `AccountService` has one constructor for injected `IAccountRestClient&` and one constructor for `RestClient&`.
- The `RestClient&` constructor owns an `AccountRestClientAdapter` in `m_ownedRest`.
- `Mql4AccountAdapter` owns an `AccountSnapshot` by value.
- INVARIANT: `Mql4AccountAdapter` never performs network I/O.
- INVARIANT: Only `AccountService` and `RestClient` paths may return `BinanceError`.

---

## 2. AccountService API

Namespace: `account`  
Source: `src/account/account_service.h`, `src/account/account_service.cpp`

### 2.A Constructors

```cpp
AccountService(IAccountRestClient& rest, AccountCompatibilityConfig compatibility);
AccountService(RestClient& rest, AccountCompatibilityConfig compatibility);
```

- POSTCONDITION: compatibility config is stored by value.
- POSTCONDITION: `RestClient&` constructor wraps the client in `AccountRestClientAdapter`.

### 2.B `snapshot`

```cpp
boost::asio::awaitable<AccountServiceResult<AccountSnapshot>>
AccountService::snapshot(AccountSnapshotRequest request = {});
```

- Always calls `m_rest.account()` first unless `request.includeAccountConfig == true`.
- REST through adapter: `GET /fapi/v2/account`.
- Optional REST when `includeBalanceEndpoint`: `GET /fapi/v2/balance`.
- Optional REST when `includePositions`: `GET /fapi/v2/positionRisk`.
- Optional position query parameter: `symbol=<upper(symbol)>`, handled by `RestClient`.
- PRECONDITION: `includeAccountConfig` must be `false` in the current implementation.
- ERROR: `includeAccountConfig == true` returns `AccountMappingError::Unsupported` before any REST call.
- ERROR: REST failures are propagated as `BinanceError` inside `AccountServiceError`.
- POSTCONDITION: `capturedAt = std::chrono::system_clock::now()` on successful snapshot.
- POSTCONDITION: `compatibility` equals the config passed to the service constructor.
- POSTCONDITION: `account` contains the result of `m_rest.account()`.

### 2.C `checkFreeMargin`

```cpp
boost::asio::awaitable<AccountServiceResult<MarginCheckResult>>
AccountService::checkFreeMargin(MarginCheckDraft draft);
```

- Current implementation status: Phase C stub.
- Does not call REST.
- POSTCONDITION: result is success, not an error.
- POSTCONDITION: `result.symbol == draft.symbol`.
- POSTCONDITION: `result.side == draft.side`.
- POSTCONDITION: `result.quantity == draft.quantity`.
- POSTCONDITION: `result.completeness == MarginCheckCompleteness::Unavailable`.
- POSTCONDITION: `result.serverAccepted == false`.
- INVARIANT: Do not treat this as server-validated free margin.
- WARNING: MQL4 lot semantics are not accepted; callers must pass an explicit `Quantity` (see `MarginCheckDraft::quantity`).

### 2.D `liquidationRisk`

```cpp
boost::asio::awaitable<AccountServiceResult<LiquidationRiskView>>
AccountService::liquidationRisk(std::optional<std::string> symbol = std::nullopt);
```

- Current implementation status: Phase C stub.
- Does not call REST.
- ERROR: always returns `AccountMappingError::Unsupported`.

---

## 3. Snapshot Semantics

Source: `src/account/account_snapshot.h`, `src/account/account_service.cpp`

### 3.A Request Flags

```cpp
struct AccountSnapshotRequest {
    bool includeBalanceEndpoint{false};
    bool includePositions{false};
    bool includeAccountConfig{false};
    std::optional<std::string> positionFilter;
};
```

| Flags | Calls | Completeness |
|---|---|---|
| none | `account()` | `AccountOnly` |
| `includeBalanceEndpoint` | `account()`, `balance()` | `AccountAndBalance` |
| `includePositions` | `account()`, `positions(positionFilter)` | `AccountAndPositions` |
| both balance and positions | `account()`, `balance()`, `positions(positionFilter)` | `AccountBalanceAndPositions` |
| `includeAccountConfig` | no calls | error: `Unsupported` |

- INVARIANT: Optional calls are sequential, not parallel.
- INVARIANT: A failure in any requested REST call fails the whole snapshot.
- INVARIANT: `Full` exists in the enum but is not produced by current code.

### 3.B REST Seam

Source: `src/account/irest_account_client.h`, `src/account/rest_account_client_adapter.h`, `src/account/rest_account_client_adapter.cpp`

```cpp
template <typename T>
using AccountRestResult = std::expected<T, BinanceError>;

class IAccountRestClient {
public:
    virtual boost::asio::awaitable<AccountRestResult<FuturesAccount>> account() = 0;
    virtual boost::asio::awaitable<AccountRestResult<std::vector<Balance>>> balance() = 0;
    virtual boost::asio::awaitable<AccountRestResult<std::vector<Position>>> positions(
        std::optional<std::string> symbol = {}) = 0;
};
```

`AccountRestClientAdapter` delegates exactly:

| Adapter method | Delegate |
|---|---|
| `account()` | `RestClient::account()` |
| `balance()` | `RestClient::balance()` |
| `positions(symbol)` | `RestClient::positions(std::move(symbol))` |

REST endpoints implemented in `src/rest/rest_client.cpp`:

| `RestClient` method | Endpoint |
|---|---|
| `account()` | signed `GET /fapi/v2/account` |
| `balance()` | signed `GET /fapi/v2/balance` |
| `positions(symbol)` | signed `GET /fapi/v2/positionRisk` |

---

## 4. MQL4 Adapter

Namespace: `account::mql4`  
Source: `src/account/mql4_account_adapter.h`, `src/account/mql4_account_adapter.cpp`

### 4.A Constructor

```cpp
explicit Mql4AccountAdapter(AccountSnapshot snapshot);
```

- POSTCONDITION: snapshot is moved/stored by value.
- INVARIANT: Adapter methods read only the stored snapshot.

### 4.B Display Asset Resolution

Internal helper: `displayAssetBalance(const AccountSnapshot&)`

- PRECONDITION: `snapshot.compatibility.displayAsset` non-empty.
- Lookup is case-insensitive.
- Search order:
  1. `snapshot.balances`, when present.
  2. `snapshot.account.assets`.
- ERROR: empty `displayAsset` -> `AccountMappingError::NotConfigured`.
- ERROR: no matching asset -> `AccountMappingError::SnapshotIncomplete`.
- INVARIANT: Display-asset fields are preferred over account totals.

### 4.C Double API

```cpp
AccountMappingResult<double> accountInfoDouble(AccountDoubleProperty property) const;
AccountMappingResult<double> accountBalance() const;
AccountMappingResult<double> accountCredit() const;
AccountMappingResult<double> accountEquity() const;
AccountMappingResult<double> accountFreeMargin() const;
AccountMappingResult<double> accountMargin() const;
AccountMappingResult<double> accountProfit() const;
```

| Property/method | Mapping | Error |
|---|---|---|
| `Balance` / `accountBalance()` | display asset `walletBalance` | display asset errors |
| `Credit` / `accountCredit()` | `0.0` if `creditPolicy == AssumeZero` | otherwise `Unsupported` |
| `Profit` / `accountProfit()` | display asset `unrealizedProfit` | display asset errors |
| `Equity` / `accountEquity()` | display asset `marginBalance` | display asset errors |
| `Margin` / `accountMargin()` | display asset `initialMargin` | display asset errors |
| `FreeMargin` / `accountFreeMargin()` | display asset `availableBalance` | display asset errors |
| `MarginLevel` | `equity / margin * 100.0`; `0.0` when margin is zero | propagated display asset errors |
| `MarginStopoutCall` | none | `Unsupported` |
| `MarginStopoutStop` | none | `Unsupported` |

### 4.D Integer API

```cpp
AccountMappingResult<int64_t> accountInfoInteger(AccountIntegerProperty property) const;
AccountMappingResult<int64_t> accountLeverage(std::string symbol) const;
AccountMappingResult<int64_t> accountNumber() const;
```

| Property/method | Mapping | Error |
|---|---|---|
| `Login` / `accountNumber()` | `compatibility.loginOverride` | `NotConfigured` when absent |
| `TradeMode` | `static_cast<int64_t>(compatibility.tradeMode)` | none |
| `Leverage` | none at account-wide level | `AmbiguousSymbol` |
| `accountLeverage(symbol)` | matching `Position::leverage` | empty symbol -> `AmbiguousSymbol`; missing position -> `SnapshotIncomplete` |
| `LimitOrders` | none | `Unsupported` |
| `MarginStopoutMode` | none | `Unsupported` |
| `TradeAllowed` | `account.canTrade ? 1 : 0` | none |
| `TradeExpert` | `(account.canTrade && expertTradeAllowed) ? 1 : 0` | none |

`accountLeverage(symbol)` search order:

1. `snapshot.positions`, when present.
2. `snapshot.account.positions`.

Symbol match is exact and case-sensitive in the current implementation.

### 4.E String API

```cpp
AccountMappingResult<std::string> accountInfoString(AccountStringProperty property) const;
AccountMappingResult<std::string> accountCompany() const;
AccountMappingResult<std::string> accountCurrency() const;
AccountMappingResult<std::string> accountName() const;
AccountMappingResult<std::string> accountServer() const;
```

| Property/method | Mapping | Error |
|---|---|---|
| `Name` / `accountName()` | `compatibility.accountName` | `NotConfigured` when absent |
| `Server` / `accountServer()` | `compatibility.serverName` | `NotConfigured` when empty |
| `Currency` / `accountCurrency()` | `compatibility.displayAsset` | `NotConfigured` when empty |
| `Company` / `accountCompany()` | `compatibility.company` | `NotConfigured` when empty |

---

## 5. Type Catalog

### 5.A Account Snapshot Types

```cpp
enum class AccountTradeMode {
    Demo,
    Contest,
    Real,
    Unknown
};

enum class AccountCreditPolicy {
    ExplicitOnly,
    AssumeZero
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
```

### 5.B Service Request/Result Types

```cpp
enum class AccountMappingError {
    Unsupported,
    NotConfigured,
    SnapshotIncomplete,
    AmbiguousSymbol
};

template <typename T>
using AccountMappingResult = std::expected<T, AccountMappingError>;

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
```

### 5.C MQL4 Property Enums

```cpp
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

} // namespace account::mql4
```

### 5.D Account Data Types

Source: `src/types/account.h`

```cpp
struct Balance {
    std::string asset;
    double walletBalance;
    double crossWalletBalance;
    double unrealizedProfit;
    double marginBalance;
    double maintMargin;
    double initialMargin;
    double availableBalance;
    double maxWithdrawAmount;
};

struct Position {
    std::string symbol;
    PositionSide positionSide;
    double positionAmt;
    double entryPrice;
    double markPrice;
    double unrealizedProfit;
    double liquidationPrice;
    int leverage;
    std::string marginType;
    double isolatedMargin;
    double initialMargin;
    double maintMargin;
    double notional;
};

struct FuturesAccount {
    double feeTier;
    bool canTrade;
    bool canDeposit;
    bool canWithdraw;
    double totalWalletBalance;
    double totalUnrealizedProfit;
    double totalMarginBalance;
    double totalInitialMargin;
    double totalMaintMargin;
    double availableBalance;
    double maxWithdrawAmount;
    std::vector<Balance> assets;
    std::vector<Position> positions;
};
```

- INVARIANT: Account numeric fields currently use `double` (`types/account.h` stores them as plain doubles; no multiprecision library is used).
- INVARIANT: MQL4 adapter balance/equity/free-margin/profit/margin use display-asset `Balance` fields, not `FuturesAccount` total fields.

---

## 6. Error Rules

### 6.A Error Separation

- `BinanceError`: REST/API/network/parse failures from `RestClient` or `IAccountRestClient`.
- `AccountMappingError`: semantic mapping failures, unsupported features, missing config, incomplete snapshots.
- INVARIANT: Snapshot-only MQL4 adapter methods cannot return `BinanceError`.
- INVARIANT: `AccountServiceResult<T>` may contain either error category.

### 6.B Mapping Error Meanings

| Error | Conditions in current implementation |
|---|---|
| `Unsupported` | `includeAccountConfig=true`, `liquidationRisk()`, explicit-only credit, stopout fields, limit orders, stopout mode |
| `NotConfigured` | empty `displayAsset`, empty `company`, empty `serverName`, missing `accountName`, missing `loginOverride` |
| `SnapshotIncomplete` | display asset not found; leverage symbol not found |
| `AmbiguousSymbol` | account-wide leverage request; empty symbol passed to `accountLeverage` |

### 6.C Stub Warnings

- WARNING: `checkFreeMargin()` is not a server test-order implementation.
- WARNING: `liquidationRisk()` is not a position-risk or bracket-aware implementation.
- WARNING: `dualSidePosition` and `multiAssetsMargin` fields exist but are not filled by `snapshot()` today.

---

## 7. Security and Privacy Rules

- INVARIANT: Do not log signed URLs, signatures, timestamps with signatures, or API keys.
- INVARIANT: Do not log raw account response bodies by default.
- INVARIANT: Do not log `accountName` or `loginOverride` unless the caller explicitly opts in.
- INVARIANT: Test fixtures must use fake account names, fake server names, and fake login overrides.

---

## 8. File Index

| Symbol | Source file |
|---|---|
| `AccountService` | `src/account/account_service.h`, `src/account/account_service.cpp` |
| `IAccountRestClient`, `AccountRestResult` | `src/account/irest_account_client.h` |
| `AccountRestClientAdapter` | `src/account/rest_account_client_adapter.h`, `src/account/rest_account_client_adapter.cpp` |
| `AccountSnapshot`, `AccountSnapshotRequest`, `AccountCompatibilityConfig` | `src/account/account_snapshot.h` |
| `AccountTradeMode`, `AccountCreditPolicy`, `AccountSnapshotCompleteness` | `src/account/account_snapshot.h` |
| `AccountMappingError`, `AccountMappingResult`, `AccountServiceError`, `AccountServiceResult` | `src/account/account_snapshot.h` |
| `MarginCheckDraft`, `MarginCheckResult`, `MarginCheckSide`, `MarginCheckCompleteness` | `src/account/account_snapshot.h` |
| `LiquidationRiskView`, `LiquidationRiskCompleteness` | `src/account/account_snapshot.h` |
| `Mql4AccountAdapter` | `src/account/mql4_account_adapter.h`, `src/account/mql4_account_adapter.cpp` |
| `AccountDoubleProperty`, `AccountIntegerProperty`, `AccountStringProperty` | `src/account/mql4_account_adapter.h` |
| `Balance`, `Position`, `FuturesAccount`, `LeverageResult` | `src/types/account.h` |
| `RestClient::account`, `RestClient::balance`, `RestClient::positions` | `src/rest/rest_client.h`, `src/rest/rest_client.cpp` |
| `Quantity`, `Price` | `src/orders/decimal_string.h` |
| `PositionSide` | `src/types/trade.h` |
