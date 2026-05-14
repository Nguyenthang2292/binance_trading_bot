# Account Command Reference

This document summarizes the actual API in `src/account` based on the current implementation.

## 1) Scope

The account layer is split into three parts:

- `AccountService`: async facade for account snapshots and account-related checks.
- `IAccountRestClient` / `AccountRestClientAdapter`: narrow REST seam over `RestClient::account()`, `balance()`, and `positions()`.
- `account::mql4::Mql4AccountAdapter`: snapshot-only compatibility adapter for MQL4-style account getters.

Current status:

- `AccountService::snapshot()` is implemented.
- `AccountService::checkFreeMargin()` is a Phase C stub that returns `Unavailable`.
- `AccountService::liquidationRisk()` is a Phase C stub that returns `AccountMappingError::Unsupported`.
- `Mql4AccountAdapter` never calls REST; it reads only the `AccountSnapshot` passed to its constructor.

## 2) Service Commands

| Command | Method | REST path used |
|---|---|---|
| Account snapshot | `AccountService::snapshot(AccountSnapshotRequest)` | Always `GET /fapi/v2/account` |
| Optional balance endpoint | `request.includeBalanceEndpoint = true` | Adds `GET /fapi/v2/balance` |
| Optional positions endpoint | `request.includePositions = true` | Adds `GET /fapi/v2/positionRisk` |
| Position filter | `request.positionFilter = "BTCUSDT"` | Adds `symbol=BTCUSDT` to position risk |
| Include account config | `request.includeAccountConfig = true` | Not implemented; returns `Unsupported` |
| Free margin check | `checkFreeMargin(MarginCheckDraft)` | No REST call currently; stub result |
| Liquidation risk | `liquidationRisk(optional<symbol>)` | No REST call currently; unsupported |

Snapshot behavior:

- `snapshot()` always calls `account()` first unless `includeAccountConfig=true`.
- If the account call fails, the `BinanceError` is propagated and no optional calls are made.
- If `includeBalanceEndpoint=true`, `balance()` must succeed or the whole snapshot fails.
- If `includePositions=true`, `positions(positionFilter)` must succeed or the whole snapshot fails.
- `capturedAt` is set when the snapshot is built.
- The provided `AccountCompatibilityConfig` is copied into the snapshot.
- Only `AccountService` and `RestClient` paths may return `BinanceError`; the snapshot-only `Mql4AccountAdapter` cannot.

Completeness values currently produced:

| Request flags | Completeness |
|---|---|
| none | `AccountOnly` |
| `includeBalanceEndpoint` | `AccountAndBalance` |
| `includePositions` | `AccountAndPositions` |
| both balance and positions | `AccountBalanceAndPositions` |
| `includeAccountConfig` | error: `AccountMappingError::Unsupported` |

`AccountSnapshotCompleteness::Full` exists in the type but is not produced by the current service implementation.

## 3) MQL4 Compatibility Commands

`Mql4AccountAdapter` stores an `AccountSnapshot` by value and exposes safe MQL4-style getters.

### Double Properties

| MQL4-style property | Method | Mapping | Error |
|---|---|---|---|
| Balance | `accountBalance()` / `accountInfoDouble(Balance)` | display asset `Balance::walletBalance` | display asset errors |
| Credit | `accountCredit()` / `accountInfoDouble(Credit)` | `0.0` when `creditPolicy=AssumeZero` | `Unsupported` otherwise |
| Profit | `accountProfit()` / `accountInfoDouble(Profit)` | display asset `Balance::unrealizedProfit` | display asset errors |
| Equity | `accountEquity()` / `accountInfoDouble(Equity)` | display asset `Balance::marginBalance` | display asset errors |
| Margin | `accountMargin()` / `accountInfoDouble(Margin)` | display asset `Balance::initialMargin` | display asset errors |
| Free margin | `accountFreeMargin()` / `accountInfoDouble(FreeMargin)` | display asset `Balance::availableBalance` | display asset errors |
| Margin level | `accountInfoDouble(MarginLevel)` | `equity / margin * 100`; `0.0` when margin is zero | propagated display asset errors |
| Stopout call | `accountInfoDouble(MarginStopoutCall)` | — | `Unsupported` |
| Stopout stop | `accountInfoDouble(MarginStopoutStop)` | — | `Unsupported` |

Display asset lookup is case-insensitive. It checks `snapshot.balances` first when present, then falls back to `snapshot.account.assets`.

### Integer Properties

| MQL4-style property | Method | Mapping |
|---|---|---|
| Login | `accountNumber()` / `accountInfoInteger(Login)` | `AccountCompatibilityConfig::loginOverride` |
| Trade mode | `accountInfoInteger(TradeMode)` | integer value of `AccountTradeMode` |
| Leverage | `accountInfoInteger(Leverage)` | unsupported as account-wide value; returns `AmbiguousSymbol` |
| Limit orders | `accountInfoInteger(LimitOrders)` | unsupported |
| Stopout mode | `accountInfoInteger(MarginStopoutMode)` | unsupported |
| Trade allowed | `accountInfoInteger(TradeAllowed)` | `snapshot.account.canTrade ? 1 : 0` |
| Expert trade allowed | `accountInfoInteger(TradeExpert)` | `canTrade && expertTradeAllowed ? 1 : 0` |

Use `accountLeverage(symbol)` for symbol-specific leverage. It requires a non-empty symbol and reads `Position::leverage` from `snapshot.positions` first, then `snapshot.account.positions`. Symbol match is exact and case-sensitive.

### String Properties

| MQL4-style property | Method | Mapping |
|---|---|---|
| Name | `accountName()` / `accountInfoString(Name)` | `AccountCompatibilityConfig::accountName` |
| Server | `accountServer()` / `accountInfoString(Server)` | `AccountCompatibilityConfig::serverName` |
| Currency | `accountCurrency()` / `accountInfoString(Currency)` | `AccountCompatibilityConfig::displayAsset` |
| Company | `accountCompany()` / `accountInfoString(Company)` | `AccountCompatibilityConfig::company` |

Configured values fail with `AccountMappingError::NotConfigured` when required fields are absent or empty.

## 4) Data and Error Model

Service methods return:

```cpp
AccountServiceResult<T> = std::expected<T, std::variant<BinanceError, AccountMappingError>>
```

Snapshot-only adapter methods return:

```cpp
AccountMappingResult<T> = std::expected<T, AccountMappingError>
```

Mapping errors:

| Error | Meaning |
|---|---|
| `Unsupported` | No safe Binance semantic equivalent, or feature is intentionally not implemented yet |
| `NotConfigured` | Required compatibility config field is absent |
| `SnapshotIncomplete` | Snapshot does not contain the asset or position required to answer |
| `AmbiguousSymbol` | Caller must provide a symbol, e.g. leverage lookup |

REST errors remain `BinanceError` and are only returned by service paths that call REST.

## 5) Compatibility Configuration

`AccountCompatibilityConfig` supplies values that Binance Futures does not expose in MQL4 form:

- `displayAsset`: default `USDT`; selects which asset powers balance/equity/free-margin getters.
- `company`: default `Binance`.
- `serverName`: required for `accountServer()`.
- `accountName`: optional and required only for `accountName()`.
- `loginOverride`: optional and required only for `accountNumber()`.
- `tradeMode`: returned by `accountInfoInteger(TradeMode)`.
- `expertTradeAllowed`: combined with `canTrade` for `TradeExpert`.
- `creditPolicy`: controls whether `accountCredit()` is unsupported or explicitly returns `0.0`.

## 6) Important Notes

- Account getters use `double` because the current `types/account.h` model stores account fields as doubles.
- MQL4 lot semantics are not accepted by `checkFreeMargin()`; callers must pass an explicit `Quantity`.
- Account-wide leverage is not supported because Binance Futures leverage is symbol/position-specific.
- Stopout level and stopout mode are not mapped to MQL4 values.
- `dualSidePosition` and `multiAssetsMargin` exist in `AccountSnapshot` but are not filled by `snapshot()` in the current implementation.
- Do not log account names, login overrides, signed URLs, signatures, or raw account response bodies by default.
- Test fixtures must use fake account names, fake server names, and fake login overrides.
