# MQL4 Account Functions To Account Layer Mapping

**Version:** 1.0  
**Date:** 2026-05-14  
**Status:** Approved design, ready for implementation  
**Disposition:** APPROVED - review findings resolved

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-05-14 | Initial mapping proposal from official MQL4 Account functions to the project's Binance USD-M Futures account surface; review findings resolved for implementation |

---

## 1. Muc Tieu

De xuat cac ham can map them vao du an khi can migrate logic MQL4 Account sang C++ Binance USD-M Futures SDK hien tai.

MQL4 Account functions duoc thiet ke quanh mot "current account" co deposit currency, broker server, account number, account leverage va stopout policy don le. Binance USD-M Futures khac ve model: account co nhieu asset, leverage theo symbol/position, position mode co the one-way hoac hedge, margin/free balance la gia tri exchange-computed theo current account mode.

Muc tieu cua tai lieu nay khong phai clone API MQL4 1:1. Muc tieu la:

- bo sung account compatibility layer co semantics ro rang;
- tan dung `RestClient::account()`, `balance()`, `positions()` hien co;
- tranh them hidden REST calls trong getter-style functions;
- danh dau ro `supported`, `partial`, `derived`, `configured`, va `unsupported`;
- khong dua ra gia tri account identity, leverage, stopout hoac margin check neu Binance khong cung cap semantics tuong duong.

---

## 2. Understanding Lock

### 2.1 Summary

- Design doc da duoc approve; day la reference doc cho implementation Phases A-C.
- Nguoi dung chinh la strategy/trading engine hoac code migration tu MQL4 sang SDK C++ hien tai.
- Pham vi la MQL4 Account functions tai `https://docs.mql4.com/account`.
- Repo hien co `src/types/account.h` va `RestClient::{account,balance,positions}` cho Binance USD-M Futures.
- Repo da co `orders::mql4::Mql4Adapter` cho trading/order migration, nhung Account functions chua co layer rieng.
- MQL4 "current account" va Binance USD-M Futures account khong tuong duong hoan toan, dac biet voi account number, leverage, free margin mode va stopout.
- Cac MQL4-style account getter nen doc tu explicit `AccountSnapshot`, khong am tham goi REST moi lan.

### 2.2 Assumptions

- Pham vi van la Binance USD-M Futures, khong gom Spot, COIN-M, Portfolio Margin, hay MQL4 broker thuc.
- Existing `RestClient` tiep tuc la transport/API client nen tang.
- Account compatibility layer la optional migration helper, khong thay the typed account API.
- Cac ham wrapper MQL4 co the tra `double` de match MQL4, nhung typed layer nen giu raw decimal string hoac decimal-safe representation khi mo rong.
- `AccountCurrency()` trong compatibility mode can mot `displayAsset`/`depositAsset` config vi Binance multi-assets mode khong co mot deposit currency duy nhat.
- Account identity nhu `AccountNumber()`/`ACCOUNT_LOGIN` khong duoc tu sinh bang hash hay fake id neu exchange khong tra ve numeric login.

### 2.3 Non-Functional Requirements

- **Performance:** snapshot refresh co the goi REST, nhung getter-style methods phai la pure reads tren snapshot.
- **Reliability:** moi snapshot phai co `capturedAt`, source endpoint, va completeness flags de caller biet stale/partial data.
- **Financial correctness:** khong convert MQL4 lots sang Binance quantity trong account margin check; caller phai dua `Quantity` hoac explicit sizing policy.
- **Security/privacy:** khong log account alias, server/base URL co API key query, signed query string, hoac raw response chua thong tin account neu chua redact.
- **Maintainability:** mapping phai nam trong account namespace/layer rieng; khong dua Account functions vao `orders::mql4` vi do la order lifecycle.

### 2.4 Resolved Questions

- **`src/account/` vs root:** Chon `src/account/` (Section 7). Tranh phinh `RestClient`; nhat quan voi `src/orders/` pattern.
- **Named MQL4 wrappers vs generic only:** Dung ca hai (Section 8.7). Generic `accountInfoDouble/Integer/String` cho migration code; named wrappers (`accountBalance`, `accountEquity`, ...) cho callers can typed convenience API.
- **`AccountCompatibilityConfig` co bat buoc khong:** Co, bat buoc khi construct `AccountService` va `Mql4AccountAdapter`. `displayAsset`, `company`, `serverName`, `tradeMode` phai duoc caller cung cap; `accountName` va `loginOverride` la optional.

---

## 3. References

| Source | URL | Notes |
|---|---|---|
| MQL4 Account Information | <https://docs.mql4.com/account> | Official list of Account functions |
| MQL4 Account Properties | <https://docs.mql4.com/constants/environment_state/accountinformation> | `ENUM_ACCOUNT_INFO_*`, trade mode, stopout mode |
| MQL4 AccountInfoDouble | <https://docs.mql4.com/account/accountinfodouble> | Generic double account property getter |
| MQL4 AccountInfoInteger | <https://docs.mql4.com/account/accountinfointeger> | Generic integer/bool/long account property getter |
| MQL4 AccountInfoString | <https://docs.mql4.com/account/accountinfostring> | Generic string account property getter |
| MQL4 AccountFreeMarginCheck | <https://docs.mql4.com/account/accountfreemargincheck> | Free margin after hypothetical buy/sell order; MQL4 raises not-enough-money error |
| MQL4 AccountFreeMarginMode | <https://docs.mql4.com/account/accountfreemarginmode> | Legacy free margin calculation mode values 0..3 |
| MQL4 AccountStopoutMode | <https://docs.mql4.com/account/accountstopoutmode> | Legacy stopout mode percent/money values |
| Binance Account Information V2 | <https://developers.binance.com/docs/derivatives/usds-margined-futures/account/rest-api/Account-Information-V2> | Current project uses `GET /fapi/v2/account` |
| Binance Account Information V3 | <https://developers.binance.com/docs/derivatives/usds-margined-futures/account/rest-api/Account-Information-V3> | Newer endpoint shape, useful future migration target |
| Binance Futures Account Balance V2 | <https://developers.binance.com/docs/derivatives/usds-margined-futures/account/rest-api/Futures-Account-Balance-V2> | `GET /fapi/v2/balance` |
| Binance Position Information V2 | <https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/Position-Information-V2> | `GET /fapi/v2/positionRisk`; docs recommend user data stream for timeliness |
| Binance Account Config | <https://developers.binance.com/docs/derivatives/usds-margined-futures/account/rest-api/Account-Config> | `GET /fapi/v1/accountConfig`, includes `canTrade`, `dualSidePosition`, `multiAssetsMargin` |
| Binance Position Mode | <https://developers.binance.com/docs/derivatives/usds-margined-futures/account/rest-api/Get-Current-Position-Mode> | `GET /fapi/v1/positionSide/dual` |
| Binance Multi-Assets Mode | <https://developers.binance.com/docs/derivatives/usds-margined-futures/account/rest-api/Get-Current-Multi-Assets-Mode> | `GET /fapi/v1/multiAssetsMargin` |
| Binance Test Order | <https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/New-Order-Test> | Server-side order validation without matching-engine submission |
| Binance Leverage Brackets | <https://developers.binance.com/docs/derivatives/usds-margined-futures/account/rest-api/Notional-and-Leverage-Brackets> | Maintenance margin bracket data for advisory risk views |
| Existing account types | `src/types/account.h` | `Balance`, `Position`, `FuturesAccount`, `AccountInfo`, `LeverageResult` |
| Existing REST account methods | `src/rest/rest_client.h` | `account()`, `balance()`, `positions()` |
| Existing MQL4 order adapter | `src/orders/mql4_adapter.h` | Current MQL4 migration surface for order placement |

---

## 4. Current Project State

### 4.1 Existing Account Support

The project already has:

- `RestClient::account()` using `GET /fapi/v2/account`.
- `RestClient::balance()` using `GET /fapi/v2/balance`.
- `RestClient::positions(symbol)` using `GET /fapi/v2/positionRisk`.
- `src/types/account.h` with `FuturesAccount`, `Balance`, `Position`, and `LeverageResult`.
- `BinanceAPI::getAccountInfo()` legacy helper that returns a simplified `AccountInfo`.

This is enough for a first account snapshot facade. It is not enough for exact MQL4 compatibility because several MQL4 values are missing or do not exist in Binance USD-M Futures.

### 4.2 Existing MQL4 Support

`orders::mql4::Mql4Adapter` currently maps MQL4-like `OrderSend` intent to typed `Orders` calls. That adapter should remain order-specific.

Account functions should use a separate namespace, for example:

```cpp
namespace account::mql4 {
class Mql4AccountAdapter;
}
```

Do not put account balance/equity/free-margin helpers under `orders::mql4`.

---

## 5. Critical Semantic Differences

### 5.1 MQL4 Has One Deposit Currency; Binance Can Be Multi-Asset

MQL4 `AccountCurrency()` returns the account deposit currency. Binance USD-M Futures can operate in single-asset or multi-assets mode. In multi-assets mode, account totals may be USD-denominated aggregate values while individual assets keep separate balances.

Compatibility must use an explicit `displayAsset`/`depositAsset` config and expose whether values are account-total USD values or asset-specific values.

### 5.2 Account Leverage Is Not Account-Wide On Binance

MQL4 `AccountLeverage()` returns one account leverage value. Binance leverage is symbol/position specific. Existing `Position` already has `leverage`.

Do not expose a fake account-wide leverage. Add symbol-scoped typed helpers instead.

### 5.3 Stopout Is Not A Broker-Level Percent/Money Setting

MQL4 has `AccountStopoutLevel()` and `AccountStopoutMode()` with broker-defined stopout semantics. Binance USD-M Futures uses margin maintenance, leverage brackets, liquidation price, position mode, and exchange risk engine behavior.

Do not map stopout to a synthetic percent/money value. Provide a separate `LiquidationRiskView` if needed.

### 5.4 `AccountFreeMarginCheck` Cannot Be Exact Without Exchange Risk Engine

MQL4 returns remaining free margin after a hypothetical order at current price. Binance has `order/test` for server-side validation, but it does not return a MQL4-style remaining free margin value. A local estimator can be advisory only and can be wrong during market moves or multi-assets mode.

### 5.5 Account Identity Is Not Exposed Like MQL4 Login

MQL4 `AccountNumber()`/`ACCOUNT_LOGIN` is a numeric account id. Binance account endpoints expose fields such as account alias in balance responses, but this is not a numeric MQL4 login and should not be treated as one.

---

## 6. Design Approaches

### 6.1 Recommended: Snapshot-First Account Adapter

Add a typed account snapshot service and optional MQL4 account adapter over that snapshot.

Key properties:

- REST refresh is explicit.
- Getter methods read from an immutable snapshot.
- Unsupported mappings return `std::unexpected(BinanceError)` or a mapping status, not fake values.
- MQL4 wrapper methods are convenience helpers, not the primary account API.

This matches the existing order design principle: avoid hidden global state and hidden REST calls.

### 6.2 Alternative: Add Only RestClient Convenience Methods

Add direct `RestClient::accountBalance()`, `accountEquity()`, etc.

This is simpler but weaker:

- encourages hidden network calls per getter;
- duplicates mapping rules in callers;
- makes stale/partial data invisible;
- makes multi-assets mode semantics unclear.

Not recommended as the main design.

### 6.3 Alternative: Clone MQL4 Global Functions

Add functions named exactly `AccountBalance()`, `AccountEquity()`, etc. with process-global state.

Rejected:

- unsafe in async C++ coroutine context;
- hides stale data;
- encourages false equivalence with MQL4 broker account semantics;
- hard to test and hard to reason about across multiple accounts/environments.

---

## 7. Proposed Module Layout

Add a small account domain layer:

```text
src/
  account/
    account_service.h
    account_service.cpp
    account_snapshot.h
    account_properties.h
    mql4_account_adapter.h
    mql4_account_adapter.cpp

tests/
  test_mql4_account.cpp
  test_account_snapshot.cpp
```

If the project wants fewer folders, the same files can be placed as `src/account_service.*`, but `src/account/` is cleaner because order compatibility already lives under `src/orders/`.

---

## 8. Proposed Types

### 8.1 Compatibility Config

```cpp
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
```

`serverName` should be derived from `ContextConfig` or explicitly configured. `loginOverride` exists only for migration environments that need to preserve a known external account id. The SDK must not synthesize one.

### 8.2 Snapshot Request

```cpp
struct AccountSnapshotRequest {
    bool includeBalanceEndpoint{false};    // call /fapi/v2/balance in addition to account.assets
    bool includePositions{false};          // call /fapi/v2/positionRisk
    bool includeAccountConfig{false};      // Phase D: call /fapi/v1/accountConfig for canTrade, dualSidePosition
    std::optional<Symbol> positionFilter;  // scope positionRisk to single symbol when set
};
```

### 8.3 Snapshot

```cpp
enum class AccountSnapshotCompleteness {
    AccountOnly,
    AccountAndBalance,
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

For v1, `account` can come from existing `RestClient::account()`. `balances` and `positions` can either come from `account.assets`/`account.positions` or explicit `balance()`/`positions()` calls when caller passes `AccountSnapshotRequest` flags.

`includeAccountConfig` is intentionally present in the request type but reserved for Phase D. Before `RestClient::accountConfig()` exists, `snapshot({.includeAccountConfig = true})` must fail explicitly with `AccountMappingError::Unsupported`; it must not silently ignore the flag.

### 8.4 MQL4 Property Enums

```cpp
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
```

Do not expose unsupported MQL4 constants as successful values just because a Binance field has a similar name.

### 8.5 Result And Error Types

Keep exchange failures and mapping failures separate:

```cpp
enum class AccountMappingError {
    Unsupported,          // no safe Binance semantic equivalent
    NotConfigured,        // required AccountCompatibilityConfig field not set
    SnapshotIncomplete,   // snapshot does not contain the data required for this property
    AmbiguousSymbol       // caller must supply a symbol (e.g. accountLeverage)
};

using AccountServiceError = std::variant<BinanceError, AccountMappingError>;

template <typename T>
using AccountMappingResult = std::expected<T, AccountMappingError>;

template <typename T>
using AccountServiceResult = std::expected<T, AccountServiceError>;
```

`AccountMappingResult<T>` is for snapshot-only adapter methods that cannot perform network I/O. `AccountServiceResult<T>` is for methods that may call REST and therefore can fail with either `BinanceError` or `AccountMappingError`.

### 8.6 Liquidation Risk View

This is not a MQL4 stopout emulation. It is a future Binance risk view with a compile-ready placeholder shape.

```cpp
enum class LiquidationRiskCompleteness {
    Unavailable,
    PositionOnly,
    BracketAware
};

struct LiquidationRiskView {
    std::optional<Symbol> symbol;
    LiquidationRiskCompleteness completeness{LiquidationRiskCompleteness::Unavailable};
    std::vector<Position> positions;
    std::optional<std::string> note;
};
```

### 8.7 Adapter Shape

```cpp
namespace account::mql4 {

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
};

} // namespace account::mql4
```

Notably absent from the basic adapter:

- no account-wide `accountLeverage()` without symbol;
- no successful `accountStopoutLevel()` / `accountStopoutMode()`;
- no exact `accountFreeMarginCheck()` until a separate advisory margin-check design exists.

---

## 9. Mapping Matrix: MQL4 Functions

Status meanings:

- **Current:** already available through existing project data, but may need facade.
- **Proposed:** add explicit helper/API.
- **Partial:** only some MQL4 semantics map safely.
- **Derived:** computed from snapshot fields.
- **Configured:** comes from `AccountCompatibilityConfig`, not exchange truth.
- **Unsupported:** no safe direct mapping.

| MQL4 function | Status | Current / proposed mapping | Notes |
|---|---|---|---|
| `AccountInfoDouble(property_id)` | Proposed, partial | `Mql4AccountAdapter::accountInfoDouble(AccountDoubleProperty)` | Reads from `AccountSnapshot`; unsupported properties fail explicitly |
| `AccountInfoInteger(property_id)` | Proposed, partial | `Mql4AccountAdapter::accountInfoInteger(AccountIntegerProperty)` | `TradeAllowed` maps best; leverage/login/stopout are constrained |
| `AccountInfoString(property_id)` | Proposed, partial | `Mql4AccountAdapter::accountInfoString(AccountStringProperty)` | Mostly configured values |
| `AccountBalance()` | Proposed, partial | `FuturesAccount::totalWalletBalance` or configured asset `Balance::walletBalance` | Must state whether total USD value or asset-specific balance |
| `AccountCredit()` | Configured fallback / unsupported direct | `AccountCompatibilityConfig::creditPolicy` | `ExplicitOnly` returns `AccountMappingError::Unsupported`; `AssumeZero` returns `0.0` by explicit opt-in |
| `AccountCompany()` | Configured | `AccountCompatibilityConfig::company`, default `Binance` | Not exchange-returned account field |
| `AccountCurrency()` | Configured, partial | `AccountCompatibilityConfig::displayAsset` | Multi-assets mode has no single MQL4 deposit currency |
| `AccountEquity()` | Proposed, partial | `FuturesAccount::totalMarginBalance` or asset `Balance::marginBalance` | MQL4 equity roughly balance + floating PnL + credit; Binance value is exchange-computed |
| `AccountFreeMargin()` | Proposed, partial | `FuturesAccount::availableBalance` or asset `Balance::availableBalance` | In multi-assets mode may be USD aggregate |
| `AccountFreeMarginCheck(symbol, cmd, volume)` | Proposed, advisory only | `AccountService::checkFreeMargin(MarginCheckDraft)` plus optional `RestClient::testOrder()` | Requires `Quantity`, not MQL4 lots; cannot return exact remaining free margin from Binance |
| `AccountFreeMarginMode()` | Unsupported direct | none | MQL4 0..3 floating P/L policy is not a Binance account setting |
| `AccountLeverage()` | Unsupported account-wide; proposed symbol helper | `accountLeverage(symbol)` from `Position::leverage` or symbol config endpoint | Binance leverage is per symbol/position, not account-wide |
| `AccountMargin()` | Proposed, partial | `FuturesAccount::totalInitialMargin`; optionally asset `initialMargin` | MQL4 "margin value" is not identical to Binance initial/maintenance split |
| `AccountName()` | Configured / unsupported | `AccountCompatibilityConfig::accountName` | Binance should not expose personal account name through this adapter by default |
| `AccountNumber()` | Configured / unsupported | `loginOverride` only | Do not map `accountAlias` to numeric login |
| `AccountProfit()` | Proposed, partial | `FuturesAccount::totalUnrealizedProfit` | Current unrealized account PnL; realized PnL belongs to trades/income history |
| `AccountServer()` | Configured | `AccountCompatibilityConfig::serverName` | Example: production or testnet base URL label |
| `AccountStopoutLevel()` | Unsupported direct | none; future `LiquidationRiskView` | Binance exposes liquidation/risk data differently |
| `AccountStopoutMode()` | Unsupported direct | none | No MQL4 percent/money stopout mode equivalent |

---

## 10. Mapping Matrix: Generic Property IDs

### 10.1 `AccountInfoDouble`

| MQL4 property | Status | Mapping |
|---|---|---|
| `ACCOUNT_BALANCE` | Partial | Same as `AccountBalance()` |
| `ACCOUNT_CREDIT` | Configured fallback / unsupported direct | Same as `AccountCredit()` |
| `ACCOUNT_PROFIT` | Partial | Same as `AccountProfit()` |
| `ACCOUNT_EQUITY` | Partial | Same as `AccountEquity()` |
| `ACCOUNT_MARGIN` | Partial | Same as `AccountMargin()` |
| `ACCOUNT_MARGIN_FREE` | Partial | Same as `AccountFreeMargin()` |
| `ACCOUNT_MARGIN_LEVEL` | Derived, partial | `equity / margin * 100` when margin > 0; otherwise unavailable or infinity policy must be explicit |
| `ACCOUNT_MARGIN_SO_CALL` | Unsupported direct | No MQL4 margin call level equivalent |
| `ACCOUNT_MARGIN_SO_SO` | Unsupported direct | No MQL4 stopout level equivalent |
| `ACCOUNT_MARGIN_INITIAL` | Unsupported for MQL4 compatibility | MQL4 docs mark not supported; typed Binance account may expose initial margin separately |
| `ACCOUNT_MARGIN_MAINTENANCE` | Unsupported for MQL4 compatibility | MQL4 docs mark not supported; typed Binance account may expose maintenance margin separately |
| `ACCOUNT_ASSETS` | Unsupported for MQL4 compatibility | MQL4 docs mark not supported |
| `ACCOUNT_LIABILITIES` | Unsupported for MQL4 compatibility | MQL4 docs mark not supported |
| `ACCOUNT_COMMISSION_BLOCKED` | Unsupported for MQL4 compatibility | MQL4 docs mark not supported |

### 10.2 `AccountInfoInteger`

| MQL4 property | Status | Mapping |
|---|---|---|
| `ACCOUNT_LOGIN` | Configured / unsupported | `loginOverride` only |
| `ACCOUNT_TRADE_MODE` | Configured | `AccountCompatibilityConfig::tradeMode`; derive default from testnet/prod only if config allows |
| `ACCOUNT_LEVERAGE` | Unsupported account-wide | Use `accountLeverage(symbol)` typed helper |
| `ACCOUNT_LIMIT_ORDERS` | Unsupported direct | Binance has symbol/exchange filters and order rate limits, not same account-wide limit |
| `ACCOUNT_MARGIN_SO_MODE` | Unsupported direct | No MQL4 stopout mode equivalent |
| `ACCOUNT_TRADE_ALLOWED` | Current, partial | `FuturesAccount::canTrade` or `accountConfig.canTrade` |
| `ACCOUNT_TRADE_EXPERT` | Configured, partial | `canTrade && compatibility.expertTradeAllowed` |

### 10.3 `AccountInfoString`

| MQL4 property | Status | Mapping |
|---|---|---|
| `ACCOUNT_NAME` | Configured / unsupported | `accountName` only |
| `ACCOUNT_SERVER` | Configured | `serverName` |
| `ACCOUNT_CURRENCY` | Configured, partial | `displayAsset` |
| `ACCOUNT_COMPANY` | Configured | `company` |

---

## 11. Proposed `AccountService`

`AccountService` should own refresh logic and expose typed snapshots:

```cpp
class AccountService {
public:
    AccountService(RestClient& rest, AccountCompatibilityConfig compatibility);

    boost::asio::awaitable<AccountServiceResult<AccountSnapshot>> snapshot(AccountSnapshotRequest request = {});
    boost::asio::awaitable<AccountServiceResult<MarginCheckResult>> checkFreeMargin(MarginCheckDraft draft);
    boost::asio::awaitable<AccountServiceResult<LiquidationRiskView>> liquidationRisk(std::optional<Symbol> symbol = std::nullopt);
};
```

`snapshot()` can initially call only existing methods:

- `RestClient::account()` for account totals, assets, positions;
- optionally `RestClient::balance()` if caller wants balance endpoint shape;
- optionally `RestClient::positions(symbol)` if caller wants fresh position risk.

In Phase C, `snapshot()` supports `includeBalanceEndpoint` and `includePositions`. `includeAccountConfig` is reserved for Phase D and must return `AccountMappingError::Unsupported` if requested before `RestClient::accountConfig()` exists.

Future endpoint additions:

- `RestClient::accountConfig()` for `canTrade`, `dualSidePosition`, `multiAssetsMargin`;
- `RestClient::positionMode()` if caller only needs mode;
- `RestClient::multiAssetsMode()` if caller only needs asset mode;
- `RestClient::testOrder()` for server-side order validation;
- `RestClient::leverageBrackets()` for advisory risk calculations.

---

## 12. Margin Check Design

MQL4:

```mql4
double AccountFreeMarginCheck(string symbol, int cmd, double volume);
```

Recommended project shape:

```cpp
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
    Symbol symbol;
    MarginCheckSide side;
    Quantity quantity;
    std::optional<Price> assumedPrice;
    bool useServerTestOrder{true};
};

struct MarginCheckResult {
    Symbol symbol;
    MarginCheckSide side;
    Quantity quantity;
    MarginCheckCompleteness completeness{MarginCheckCompleteness::Unavailable};
    std::optional<std::string> estimatedRemainingFreeMargin;  // string, not double: avoids binary float rounding on an advisory financial value
    bool serverAccepted{false};
    std::optional<int> binanceCode;
    std::optional<std::string> binanceMessage;
};
```

Rules:

- Reject MQL4 `volume`/lots unless caller supplies an explicit lot sizing policy that returns `Quantity`.
- `cmd` supports only buy/sell equivalent, not pending order variants for free margin check v1.
- `testOrder` success means server accepted the order shape at validation time; it does not mean remaining free margin is known.
- Local estimate must be labelled advisory and should require current account snapshot plus price source.

---

## 13. Error Handling

Do not return MQL4-style sentinel values silently.

Recommended behavior:

- unsupported mapping returns a dedicated mapping error, not a Binance API error (see error taxonomy below);
- configured-but-missing values return explicit error, e.g. `account_name_not_configured`;
- `AccountCredit()` behavior is controlled by `AccountCompatibilityConfig::creditPolicy`: `ExplicitOnly` returns error, `AssumeZero` returns `0.0`;
- `AccountNumber()` should not return `0` unless caller explicitly asks for legacy sentinel behavior.

### Error taxonomy

Two distinct error categories must not be conflated:

- **`BinanceError`** — API/network errors: HTTP failures, exchange error codes, timeout, parse failure. These come from `RestClient` and represent exchange-side problems.
- **`AccountMappingError`** — semantic mapping failures: a MQL4 concept has no safe Binance equivalent, a required config field is absent, or the snapshot lacks the data needed to answer the query.

The concrete result aliases are defined in Section 8.5.

- Snapshot-only adapter methods return `AccountMappingResult<T>` because they cannot call REST.
- Service methods return `AccountServiceResult<T>` because they may call REST and may also reject unsupported mapping requests.
- Callers must not receive a `BinanceError` for a purely semantic mapping failure.

If a wrapper must mimic MQL4 return types for migration, add a thin legacy facade over the safe adapter, not in the core account API.

---

## 14. Security And Privacy

Account data is more sensitive than normal order metadata.

Required guardrails:

- redact raw account response bodies by default;
- do not log `accountAlias`, configured account name, or login override unless caller opts in;
- never log API key, signature, signed query string, or full URL with signed params;
- keep `AccountName()` configured-only and absent by default;
- ensure test fixtures use fake account aliases and fake server names.

---

## 15. Testing Strategy

Add focused tests before implementation is considered complete:

- `AccountSnapshot` can be built from existing `FuturesAccount` fixtures.
- `AccountBalance`, `AccountEquity`, `AccountFreeMargin`, `AccountMargin`, and `AccountProfit` read only from snapshot.
- `AccountInfoDouble` maps every supported property and rejects unsupported properties.
- `AccountInfoInteger(ACCOUNT_TRADE_ALLOWED)` follows `canTrade`; `ACCOUNT_TRADE_EXPERT` follows `canTrade && expertTradeAllowed`.
- `AccountLeverage(symbol)` reads the correct `Position::leverage` and rejects missing/ambiguous symbol.
- Multi-assets mode requires explicit `displayAsset` semantics in output docs/tests.
- `AccountNumber()` fails when `loginOverride` is absent.
- `AccountFreeMarginCheck` rejects lot-like input and returns advisory completeness when no server test order endpoint is available.
- Getter methods do not call REST; only `AccountService::snapshot()` and explicit service methods can call REST.

---

## 16. Phased Implementation Plan

### Phase A - Account Snapshot Types

- Add `src/account/account_snapshot.h`.
- Add `AccountCompatibilityConfig`.
- Add snapshot construction from existing `FuturesAccount`.
- Tests use local fixtures only.

### Phase B - MQL4 Account Adapter

- Add `account::mql4::Mql4AccountAdapter`.
- Implement generic `accountInfoDouble`, `accountInfoInteger`, `accountInfoString`.
- Implement safe wrappers for balance/equity/free margin/margin/profit/company/currency/server/name/number.
- Keep unsupported leverage/account number/stopout behavior explicit.

### Phase C - AccountService

- Add `AccountService` with the **full declared interface**: `snapshot()`, `checkFreeMargin()`, and `liquidationRisk()`.
- `snapshot(AccountSnapshotRequest)` calls `RestClient::account()`; calls `balance()` and `positions()` only when the corresponding request flags are set. `includeAccountConfig=true` returns `AccountMappingError::Unsupported` until Phase D.
- `checkFreeMargin()` and `liquidationRisk()` are declared and compiled in Phase C. The stub return styles differ intentionally: `checkFreeMargin()` returns a **successful** `MarginCheckResult{.completeness = Unavailable}` because migration callers are expected to check `completeness` rather than handle an error path; `liquidationRisk()` returns `AccountMappingError::Unsupported` because there is no MQL4 equivalent to migrate and no caller should depend on it before Phase E. Callers can code against the full interface now.
- Add `capturedAt` and `completeness` to snapshot.

### Phase D - Optional Binance Endpoint Expansion

- Add `accountConfig()`, `positionMode()`, `multiAssetsMode()`, `testOrder()`, and `leverageBrackets()` only if callers need stronger semantics.
- Keep these endpoints typed and separately tested.

### Phase E - Advisory Margin And Risk Views

- Implement `checkFreeMargin()` as advisory/server-validation hybrid (replaces stub from Phase C).
- Implement `liquidationRisk()` with `LiquidationRiskView` instead of mapping stopout directly.
- Document that these are Binance risk views, not MQL4 stopout emulation.

---

## 17. Decision Log

| Decision | Alternatives considered | Rationale | Status |
|---|---|---|---|
| Add account mapping as a separate account layer | Put Account functions into `orders::mql4` | Account state is not order lifecycle; separate ownership avoids coupling | Approved |
| Use explicit snapshots for getter-style functions | Call REST inside every getter | Avoid hidden network cost, stale ambiguity, and async race patterns | Approved |
| Treat account-wide leverage as unsupported | Return first position leverage or configured default | Binance leverage is symbol/position scoped; fake account leverage is dangerous | Approved |
| Treat stopout functions as unsupported direct | Derive from liquidation price or maintenance margin | Binance risk model is not MQL4 broker stopout percent/money mode | Approved |
| Make `AccountCurrency` configured | Infer from first asset | Multi-assets mode can have several assets and USD aggregate totals | Approved |
| Do not map account alias to account number | Parse/hash `accountAlias` | MQL4 expects numeric login; alias is not equivalent and may be sensitive | Approved |
| Make `AccountFreeMarginCheck` advisory only | Return exact remaining margin | Binance `order/test` validates order shape but does not return MQL4 remaining free margin | Approved |

---

## 18. Recommendation

Implement only Phases A-C first:

1. `AccountSnapshot` and `AccountCompatibilityConfig`.
2. `account::mql4::Mql4AccountAdapter` over a snapshot.
3. `AccountService::snapshot()` using existing `RestClient::account()`.

Defer exact margin/risk expansion until there is a caller that needs it. `AccountFreeMarginCheck`, `AccountStopoutLevel`, and `AccountStopoutMode` are the main danger areas; implementing them as if they were exact would make the SDK less safe.
