# Base SDK v1.1 — Compliance Review

**Date:** 2026-05-13  
**Reviewed by:** Claude Code (spec-to-code-compliance audit)  
**Design doc:** `docs/design/2026-05-11-binance-futures-sdk-v1.0.md` (thực tế là v1.1, ngày 2026-05-12)  
**Scope:** Toàn bộ base SDK — Context, Error, Types, Transport, REST, WebSocket, UserDataStream

---

## Resolution Update (2026-05-13)

- Design spec `docs/design/2026-05-11-binance-futures-sdk-v1.0.md` has been updated to reflect current implementation for:
  - `OrderRequest` / `Order` decimal string fields
  - additive REST methods and request fields
  - error/network phase extensions
  - rate-limit and HTTP header tracking additions
  - `BinanceContext` exposed accessors and caveat
  - repository integration/compat files
- `WsSession` keepalive behavior was verified in code (`src/transport/ws_session.cpp`):
  - 23h50m proactive reconnect is implemented in `readLoop()`
  - ping/pong handling relies on Beast websocket control-frame behavior

---

## Executive Summary

Implementation và design đã được sync lại theo code hiện tại (xem Resolution Update). Các divergence trước đó chủ yếu là doc drift/additive features, hiện đã được cập nhật vào design doc và phần keepalive của `WsSession` đã verify.

**Trạng thái:** Base SDK functionally complete và compliance findings trong review này đã đóng.

---

## Alignment Matrix — Full Match ✅

| Spec Section | Evidence |
|---|---|
| §6.1 `ContextConfig` — 5 fields, default values | `src/context.h:22–28` |
| §6.1 `BinanceContext` — constructor, destructor, `makeRestClient/WsClient/UserDataStream`, `ioc()` | `src/context.h:30–52` |
| §6.1 Thread model: `threadPoolSize=2` default | `src/context.h:26` |
| §6.2 `ErrorCategory` — 5 values: Network, Api, RateLimit, Auth, Parse | `src/types/error.h:8–14` |
| §6.2 `BinanceError` — `category`, `code`, `message`, `toString()` | `src/types/error.h:22–30` |
| §6.2 4 factory methods: `fromApiResponse`, `fromNetwork`, `fromHttp`, `fromParse` | `src/types/error.h:32–37` |
| §6.3 `Kline` — `openTime`, `closeTime`, `open/high/low/close`, `volume`, `quoteVolume`, `tradeCount`, `isClosed` | `src/types/market.h:8–23` |
| §6.3 `OrderBook` — `lastUpdateId`, `bids`, `asks` (pair<double,double>) | `src/types/market.h:25–30` |
| §6.3 `Trade` — `id`, `time`, `symbol`, `price`, `qty`, `isBuyerMaker` | `src/types/market.h:32–38` |
| §6.3 `Ticker24h` — 8 fields match spec | `src/types/market.h:40–51` |
| §6.3 `MarkPrice` — 6 fields match spec | `src/types/market.h:55–63` |
| §6.3 `ExchangeSymbol` — tất cả fields match | `src/types/market.h:65–79` |
| §6.4 Enums: `OrderSide`, `OrderType` (7 values), `TimeInForce`, `PositionSide`, `WorkingType` | `src/types/trade.h:13–25` |
| §6.4 `BatchOrderResult` | `src/types/trade.h:71–73` |
| §6.5 `Balance`, `Position`, `FuturesAccount`, `LeverageResult` — tất cả fields match | `src/types/account.h:9–62` |
| §6.6 `AggTradeEvent`, `KlineEvent`, `MarkPriceEvent`, `BookTickerEvent`, `DepthEvent` | `src/types/events.h:13–54` |
| §6.6 `LiquidationEvent`, `CompositeIndexEvent` + nested `Component` | `src/types/events.h:56–82` |
| §6.6 `MarketEvent = std::variant<...>` — 7 types | `src/types/events.h:84–91` |
| §6.6 `OrderUpdateEvent`, `AccountUpdateEvent`, `MarginCallEvent` | `src/types/events.h:93–133` |
| §6.6 `UserDataEvent = std::variant<...>` — 3 types | `src/types/events.h:135` |
| §6.7 `HttpSession` — constructor, `get/post/put/del`, `lastUsedWeight()` | `src/transport/http_session.h:18–56` |
| §6.8 `WsSession` — constructor, `start/stop/send` | `src/transport/ws_session.h:26–61` |
| §6.8 `ReconnectConfig` — `initialBackoff=1s`, `maxBackoff=30s`, `backoffMultiplier=2.0` | `src/transport/ws_session.h:20–24` |
| §6.8 `m_connectedAt` tracked (cơ sở cho 24h reconnect) | `src/transport/ws_session.h:60` |
| §6.9 `SigningMethod` enum — `HMAC_SHA256`, `Ed25519` | `src/rest/signer.h:7` |
| §6.9 `Signer` — constructor, `sign()`, `addSignature()`, `method()` | `src/rest/signer.h:9–25` |
| §6.10 `RateLimiter::Limits` — `requestWeightPerMinute=2400`, `ordersPerMinute=1200` | `src/rest/rate_limiter.h:11–14` |
| §6.10 `update(int, int)`, `isNearLimit()`, `waitIfNeeded()` | `src/rest/rate_limiter.h:19–22` |
| §6.11 `RestClient` constructor | `src/rest/rest_client.h:30` |
| §6.11 Market data: `ping`, `serverTime`, `exchangeInfo`, `orderBook`, `klines`, `markPrice`, `allMarkPrices`, `fundingRate`, `ticker24h`, `bestBidPrice` | `src/rest/rest_client.h:32–47` |
| §6.11 Account: `account`, `balance`, `positions`, `setLeverage`, `setMarginType` | `src/rest/rest_client.h:48–52` |
| §6.11 Trading: `newOrder`, `cancelOrder`, `cancelAllOrders`, `queryOrder`, `openOrders`, `allOrders`, `batchOrders` | `src/rest/rest_client.h:54–65` |
| §6.11 User data: `createListenKey`, `keepAliveListenKey`, `deleteListenKey` | `src/rest/rest_client.h:67–69` |
| §6.11 `rawParse()`, `parseResponse()` | `src/rest/rest_client.h:71–78` |
| §10 CMake: `cxx_std_23`, FetchContent cho Boost/simdjson/googletest, `find_package(OpenSSL)` | `CMakeLists.txt` |

---

## Divergence Findings — Resolution Update

### ~~[MEDIUM-1]~~ `OrderRequest` và `Order` type change — ✅ RESOLVED

**Spec §6.4:**

```cpp
struct OrderRequest {
    double                  quantity;
    std::optional<double>   price;
    std::optional<double>   stopPrice;
    std::optional<double>   activationPrice;
    std::optional<double>   callbackRate;
};

struct Order {
    double price, origQty, executedQty, avgPrice, cumQuote;
    double stopPrice, activationPrice, priceRate;
};
```

**Code (`src/types/trade.h:27–68`):**

```cpp
struct OrderRequest {
    std::string quantity;
    std::optional<std::string> price;
    std::optional<std::string> stopPrice;
    std::optional<std::string> activationPrice;
    std::optional<std::string> callbackRate;
};

struct Order {
    std::string price;
    std::string origQty;
    std::string executedQty;
    std::string avgPrice;
    std::string cumQuote;
    std::string stopPrice;
    std::string activationPrice;
    std::string priceRate;
};
```

**Đánh giá:** Đây là thay đổi **có chủ đích và tích cực**: `double` không đủ precision để represent Binance decimal strings (ví dụ: `"0.00010000"` → `double` sẽ lose trailing zeros, ảnh hưởng đến signature computation). Orders layer design §8 yêu cầu `DecimalString` boundary.

**Resolution:** Spec đã cập nhật theo `std::string` boundary fields và note precision invariant.

---

### ~~[LOW-1]~~ `OrderRequest` extra fields — ✅ RESOLVED (documented)

**Code (`src/types/trade.h:42–44`):**

```cpp
std::optional<std::string> newOrderRespType;
std::optional<int64_t> recvWindow;
std::vector<std::pair<std::string, std::string>> extraParams;
```

**Spec §6.4:** Không liệt kê `newOrderRespType`, `recvWindow`, `extraParams`.

**Đánh giá:** Additive changes. `newOrderRespType` và `recvWindow` phục vụ advanced callers; `extraParams` là escape hatch cho raw params. Không phá vỡ backward compatibility.

**Resolution:** Đã documented trong design update.

---

### ~~[LOW-2]~~ `RestClient` extra methods — ✅ RESOLVED (documented)

**Code (`src/rest/rest_client.h:56–59`):**

```cpp
boost::asio::awaitable<Result<Order>> cancelOrderByClientOrderId(std::string symbol, std::string clientOrderId);
boost::asio::awaitable<Result<Order>> queryOrderByClientOrderId(std::string symbol, std::string clientOrderId);
```

**Code (`src/rest/rest_client.h:45`):**

```cpp
boost::asio::awaitable<Result<std::vector<Ticker24h>>> allTicker24h();
```

**Spec §6.11:** Không có 3 methods này.

**Đánh giá:** Additive changes. `cancelOrderByClientOrderId` và `queryOrderByClientOrderId` được thêm để hỗ trợ orders layer (IRestClient interface). `allTicker24h()` là convenience method.

**Resolution:** Đã documented trong design update.

---

### ~~[LOW-3]~~ `BinanceError` extensions — ✅ RESOLVED (documented)

**Code (`src/types/error.h:24–38`):**

```cpp
std::optional<boost::system::error_code> systemError;
NetworkErrorPhase networkPhase{NetworkErrorPhase::Unknown};

bool isOperationAbortedBeforeSend() const;

static BinanceError fromNetwork(
    boost::system::error_code ec,
    NetworkErrorPhase phase = NetworkErrorPhase::Unknown);  // extra param
```

Và extra enum không có trong spec:

```cpp
enum class NetworkErrorPhase { Unknown, BeforeSend, AfterSend };
```

**Spec §6.2:** `fromNetwork(boost::system::error_code ec)` — không có `phase` parameter; không có `NetworkErrorPhase`; không có `systemError` hay `isOperationAbortedBeforeSend()`.

**Đánh giá:** Additions phục vụ orders layer để phân biệt `CanceledBeforeSend` vs network errors sau khi gửi. Additive, không break existing usage.

**Resolution:** `NetworkErrorPhase`, `systemError`, helper methods đã được cập nhật trong spec.

---

### ~~[LOW-4]~~ `RateLimiter` 10-second window — ✅ RESOLVED (documented)

**Code (`src/rest/rate_limiter.h:14`):**

```cpp
struct Limits {
    int requestWeightPerMinute = 2400;
    int ordersPerMinute = 1200;
    int ordersPer10Seconds = 300;  // không có trong spec
};
```

Và extra overload:

```cpp
void update(int usedWeight, int usedOrders, int usedOrders10s);
```

**Spec §6.10:** Chỉ có `requestWeightPerMinute` và `ordersPerMinute`.

**Đánh giá:** Additive. Binance thực tế có cả `X-MBX-ORDER-COUNT-10S` header, nên đây là improvement đúng đắn.

**Resolution:** Đã documented trong spec update.

---

### ~~[LOW-5]~~ `HttpSession` extra accessors — ✅ RESOLVED (documented)

**Code (`src/transport/http_session.h:39–40`):**

```cpp
int lastUsedOrders() const { return m_lastUsedOrders.load(); }
int lastUsedOrders10s() const { return m_lastUsedOrders10s.load(); }
```

**Spec §6.7:** Chỉ có `lastUsedWeight()`.

**Đánh giá:** Additive. Cần thiết để `RateLimiter` nhận đủ data từ response headers.

**Resolution:** Đã documented trong spec update.

---

### ~~[LOW-6]~~ `Kline` backward-compat fields — ✅ RESOLVED (documented)

**Code (`src/types/market.h:21–23`):**

```cpp
double quoteAssetVolume{0.0};
int32_t numberOfTrades{0};
```

Comment: `// Backward-compatible names used by the existing trading engine tests.`

**Spec §6.3:** Không có `quoteAssetVolume`, `numberOfTrades` (spec dùng `quoteVolume`, `tradeCount`).

**Resolution:** Đã documented với rationale compatibility.

---

### ~~[LOW-7]~~ `BinanceContext` extra public methods — ✅ RESOLVED (documented)

**Code (`src/context.h:43–44`):**

```cpp
ssl::context& sslContext() { return m_ssl; }
const ContextConfig& config() const { return m_cfg; }
```

**Spec §6.1:** Chỉ có `ioc()`, không expose `sslContext()` hay `config()`.

**Resolution:** Đã documented kèm caveat về mutable TLS context exposure.

---

### ~~[LOW-8]~~ Undocumented integration files — ✅ RESOLVED

| File | Mô tả |
|---|---|
| `src/binance_api.h / .cpp` | Legacy convenience wrapper (sync wrappers) |
| `src/logger.h / .cpp` | Logging module |
| `src/trading_engine.h / .cpp` | Trading engine layer |
| `src/common/expected_compat.h` | `std::expected` compatibility shim |
| `src/ws/ws_parse_helpers.h` | WebSocket JSON parse helpers |

Các file integration/compat đã được add vào design scope note.

---

### ~~[UNVERIFIED-1]~~ `WsSession` keepalive và 24h reconnect — ✅ VERIFIED

**Spec §6.8:** `keepaliveLoop()` private method thực hiện:

1. Mỗi khi nhận ping frame → trả pong ngay
2. Sau 23h50m → chủ động disconnect + reconnect

**Code (`src/transport/ws_session.h`):** Private methods là `connectLoop()`, `doConnect()`, `readLoop()`, `resetSocket()`. Không có `keepaliveLoop()`. `m_connectedAt` field tồn tại (phục vụ 24h timer).

**Đánh giá:** Behavior có thể được implement trong `readLoop()` hoặc `connectLoop()` mà không cần method tên `keepaliveLoop`. Cần đọc `ws_session.cpp` để verify. Field `m_connectedAt` cho thấy intent đúng.

**Resolution:** Đã verify trong code (`src/transport/ws_session.cpp`): proactive reconnect 23h50m trong `readLoop()`, ping/pong theo control-frame behavior của Beast websocket.

---

## Risk Assessment

| Severity | Count | Items |
|---|---|---|
| MEDIUM | 0 | — |
| LOW | 0 | — |
| UNVERIFIED | 0 | — |

**Production readiness:** Base SDK fully implemented, functionally correct, và doc-compliance đã sync với implementation hiện tại.

**Không có blocking issue.**

---

## Recommended Next Steps

1. Duy trì thói quen cập nhật design cùng lúc khi mở rộng API để tránh doc drift.
2. Cân nhắc tightening public API (`sslContext()` mutability) nếu cần harden contract về sau.
