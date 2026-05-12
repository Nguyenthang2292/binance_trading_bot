# Binance Futures C++ SDK — Design Document
**Version:** 1.1  
**Date:** 2026-05-12  
**Status:** Draft

---

## 1. Mục Tiêu

Xây dựng lại hoàn toàn module tương tác với Binance **USDⓈ-M Futures** (chỉ futures, không spot) bằng C++20, với:

- REST API đầy đủ qua HTTPS async (Boost.Beast)
- WebSocket market streams + User Data Stream
- Async model dựa trên `boost::asio::io_context` và C++23 coroutines (`asio::awaitable`)
- JSON parsing hiệu năng cao với **simdjson**
- Error handling: `std::expected<T, BinanceError>` (C++23 native) cho REST, callback `(error_code, event)` cho WebSocket
- Auto-reconnect WebSocket với exponential backoff + notify caller qua `onDisconnect`/`onReconnect`

---

## 2. Tham Chiếu

| Nguồn | URL |
|---|---|
| Binance USDⓈ-M Futures REST docs | https://developers.binance.com/docs/derivatives/usds-margined-futures/general-info |
| Binance WebSocket Market Streams | https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-market-streams |
| REST base URL (production) | `https://fapi.binance.com` |
| WebSocket base URL (production) | `wss://fstream.binance.com` |
| REST testnet | `https://demo-fapi.binance.com` |
| WebSocket testnet | `wss://fstream.binancefuture.com` |
| Tham khảo SDK | https://github.com/dgrr/binance-futures-sdk |

---

## 3. Dependencies

Xoá `libcurl` và `nlohmann_json`. Thêm mới:

| Thư viện | Phiên bản tối thiểu | Mục đích |
|---|---|---|
| Boost.Beast | ≥ 1.85 | HTTPS + WebSocket async |
| Boost.ASIO | ≥ 1.85 | `io_context`, coroutines |
| Boost.System | ≥ 1.85 | `error_code` |
| simdjson | ≥ 3.10 | JSON parsing tốc độ cao (SIMD) |
| OpenSSL | (đã có) | TLS, HMAC-SHA256 signing |
| GoogleTest | ≥ 1.14 | (đã có) Unit tests |

Tất cả thêm qua `FetchContent` trong CMakeLists.txt.

---

## 4. Cấu Trúc Thư Mục

```
src/
├── context.h / context.cpp              # BinanceContext — owns io_context + thread pool
│
├── types/
│   ├── error.h                          # BinanceError, ErrorCategory
│   ├── market.h                         # Kline, OrderBook, Trade, Ticker24h,
│   │                                    #   MarkPrice, FundingRate, ExchangeSymbol
│   ├── trade.h                          # Order, OrderRequest, OrderSide, OrderType,
│   │                                    #   TimeInForce, PositionSide, BatchOrderResult
│   ├── account.h                        # FuturesAccount, Position, Balance, LeverageResult
│   └── events.h                         # WS event types (market + user data)
│
├── transport/
│   ├── http_session.h / http_session.cpp  # Persistent HTTPS conn (Boost.Beast)
│   └── ws_session.h  / ws_session.cpp    # WebSocket conn + ping/pong + reconnect
│
├── rest/
│   ├── rest_client.h / rest_client.cpp  # Public REST API (coroutine-based)
│   ├── signer.h      / signer.cpp       # HMAC-SHA256 signing (OpenSSL)
│   └── rate_limiter.h / rate_limiter.cpp # REQUEST_WEIGHT + ORDER limit tracker
│
└── ws/
    ├── ws_client.h       / ws_client.cpp        # Market stream subscription manager
    └── user_data_stream.h / user_data_stream.cpp # Listen key lifecycle + account stream

tests/
├── test_signing.cpp
├── test_types.cpp
├── test_rest_client.cpp
└── test_ws_client.cpp
```

---

## 5. Kiến Trúc Tổng Thể

```
┌─────────────────────────────────────────────────────────────────────┐
│                          Application Code                           │
├─────────────────────┬────────────────────┬──────────────────────────┤
│     RestClient      │     WsClient       │     UserDataStream       │
│  (coroutine-based)  │  (callback-based)  │  (listen key + WS)       │
├─────────────────────┴────────────────────┴──────────────────────────┤
│        signer + rate_limiter        │  ws_session (reconnect logic) │
├─────────────────────────────────────┴───────────────────────────────┤
│          HttpSession (persistent HTTPS)   WsSession (WebSocket)     │
├─────────────────────────────────────────────────────────────────────┤
│                    Boost.Beast  +  Boost.ASIO                       │
│                OpenSSL (TLS + HMAC)    simdjson (JSON)              │
├─────────────────────────────────────────────────────────────────────┤
│                         BinanceContext                              │
│               io_context  +  background thread pool                 │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 6. Chi Tiết Từng Module

### 6.1 BinanceContext (`src/context.h`)

Object trung tâm. Caller tạo một lần và giữ suốt vòng đời chương trình. Owns `io_context` và thread pool.

```cpp
struct ContextConfig {
    std::string apiKey;
    std::string secretKey;
    bool        testnet        = false;
    size_t      threadPoolSize = 2;   // Default 2 cho production: đủ cho HTTP + WS concurrent processing
    SigningMethod signingMethod = SigningMethod::HMAC_SHA256;  // HMAC_SHA256 hoặc Ed25519
};

class BinanceContext {
public:
    explicit BinanceContext(ContextConfig cfg);
    ~BinanceContext();           // stop io_context, join all threads

    RestClient     makeRestClient();
    WsClient       makeWsClient();
    UserDataStream makeUserDataStream();   // tự tạo RestClient nội bộ để quản lý listen key

    asio::io_context& ioc();

private:
    ContextConfig                              m_cfg;
    asio::io_context                           m_ioc;
    asio::executor_work_guard<
        asio::io_context::executor_type>       m_work;
    ssl::context                               m_ssl;    // shared TLS context
    std::vector<std::thread>                   m_threads;
};
```

**Thread model:** `threadPoolSize = 2` là mặc định cho production (đủ cho HTTP + WS concurrent processing mà không quá nặng). Với multi-symbol hoặc throughput cao, tăng lên 3–4. Tất cả callback và coroutine đều chạy trên thread pool này — caller không cần mutex nếu chỉ dùng 1 thread. Với threadPoolSize=1, có thể xảy ra head-of-line blocking nếu một operation chờ lâu.

---

### 6.2 Error Types (`src/types/error.h`)

```cpp
enum class ErrorCategory {
    Network,    // Boost.Beast / TLS / connection errors
    Api,        // Binance trả về JSON error body {"code": -XXXX, "msg": "..."}
    RateLimit,  // HTTP 429 (soft ban) hoặc 418 (IP ban)
    Auth,       // Signature sai, API key thiếu/sai
    Parse,      // simdjson parse failure hoặc unexpected schema
};

struct BinanceError {
    ErrorCategory category;
    int           code;       // Binance error code (e.g. -1121) hoặc HTTP status
    std::string   message;

    std::string toString() const;

    static BinanceError fromApiResponse(int code, std::string_view msg);
    static BinanceError fromNetwork(boost::system::error_code ec);
    static BinanceError fromHttp(int httpStatus, std::string_view body);
    static BinanceError fromParse(std::string_view detail);
};
```

**Mapping HTTP status → category:**
- `429` → `RateLimit` (tạm thời)
- `418` → `RateLimit` (IP ban, không retry)
- `401`, `403` → `Auth`
- `4xx` khác với JSON body → `Api`
- Network/TLS failure → `Network`

---

### 6.3 Data Types — Market (`src/types/market.h`)

```cpp
struct Kline {
    int64_t openTime, closeTime;
    double  open, high, low, close;
    double  volume, quoteVolume;
    int32_t tradeCount;
    bool    isClosed;           // false khi đây là candle đang hình thành (từ WS)
};

struct OrderBook {
    int64_t                               lastUpdateId;
    std::vector<std::pair<double,double>> bids;   // {price, qty}, sorted desc
    std::vector<std::pair<double,double>> asks;   // {price, qty}, sorted asc
};

struct Trade {
    int64_t     id, time;
    std::string symbol;
    double      price, qty;
    bool        isBuyerMaker;
};

struct Ticker24h {
    std::string symbol;
    double      lastPrice;
    double      priceChange, priceChangePercent;
    double      highPrice, lowPrice;
    double      volume, quoteVolume;
    int64_t     openTime, closeTime;
};

struct MarkPrice {
    std::string symbol;
    double      markPrice, indexPrice;
    double      estimatedSettlePrice;
    double      fundingRate;
    int64_t     nextFundingTime, time;
};

struct ExchangeSymbol {
    std::string symbol, baseAsset, quoteAsset;
    std::string contractType;   // "PERPETUAL" | "CURRENT_MONTH" | ...
    std::string status;         // "TRADING" | "BREAK" | ...
    int         pricePrecision, quantityPrecision, baseAssetPrecision;
    double      tickSize;       // bước giá tối thiểu
    double      stepSize;       // bước quantity tối thiểu
    double      minNotional;    // giá trị lệnh tối thiểu (qty * price)
    double      maxQty, minQty;
};
```

---

### 6.4 Data Types — Trade (`src/types/trade.h`)

```cpp
enum class OrderSide     { Buy, Sell };
enum class OrderType     { Limit, Market, Stop, StopMarket,
                           TakeProfit, TakeProfitMarket, TrailingStopMarket };
enum class TimeInForce   { GTC, IOC, FOK, GTX };
enum class PositionSide  { Both, Long, Short };
enum class WorkingType   { MarkPrice, ContractPrice };

struct OrderRequest {
    std::string             symbol;
    OrderSide               side;
    OrderType               type;
    PositionSide            positionSide     = PositionSide::Both;
    double                  quantity;
    std::optional<double>   price;            // bắt buộc nếu type = Limit
    std::optional<double>   stopPrice;        // bắt buộc nếu type = Stop/TakeProfit
    std::optional<double>   activationPrice;  // dùng cho TrailingStopMarket
    std::optional<double>   callbackRate;     // dùng cho TrailingStopMarket (%)
    std::optional<TimeInForce> timeInForce;
    std::optional<bool>     reduceOnly;
    std::optional<bool>     closePosition;
    std::optional<WorkingType> workingType;
    std::optional<std::string> newClientOrderId;
};

struct Order {
    std::string  symbol, clientOrderId;
    int64_t      orderId;
    OrderSide    side;
    OrderType    type;
    PositionSide positionSide;
    TimeInForce  timeInForce;
    std::string  status;         // NEW | PARTIALLY_FILLED | FILLED | CANCELED | EXPIRED
    double       price, origQty, executedQty, avgPrice, cumQuote;
    bool         reduceOnly, closePosition;
    double       stopPrice, activationPrice, priceRate;
    WorkingType  workingType;
    int64_t      time, updateTime;
};

struct BatchOrderResult {
    std::vector<std::expected<Order, BinanceError>> results;
};
```

---

### 6.5 Data Types — Account (`src/types/account.h`)

```cpp
struct Balance {
    std::string asset;
    double      walletBalance;
    double      crossWalletBalance;
    double      unrealizedProfit;
    double      marginBalance;
    double      maintMargin;
    double      initialMargin;
    double      availableBalance;
    double      maxWithdrawAmount;
};

struct Position {
    std::string  symbol;
    PositionSide positionSide;
    double       positionAmt;        // dương = long, âm = short, 0 = không có vị thế
    double       entryPrice;
    double       markPrice;
    double       unrealizedProfit;
    double       liquidationPrice;
    int          leverage;
    std::string  marginType;         // "isolated" | "cross"
    double       isolatedMargin;
    double       initialMargin;
    double       maintMargin;
    double       notional;           // positionAmt * markPrice
};

struct FuturesAccount {
    double               feeTier;
    bool                 canTrade, canDeposit, canWithdraw;
    double               totalWalletBalance;
    double               totalUnrealizedProfit;
    double               totalMarginBalance;
    double               totalInitialMargin;
    double               totalMaintMargin;
    double               availableBalance;
    double               maxWithdrawAmount;
    std::vector<Balance>  assets;
    std::vector<Position> positions;
};

struct LeverageResult {
    std::string symbol;
    int         leverage;
    double      maxNotionalValue;
};
```

---

### 6.6 Data Types — WebSocket Events (`src/types/events.h`)

#### Market Events

```cpp
struct AggTradeEvent {
    std::string symbol;
    int64_t     aggTradeId, time;
    double      price, qty;
    bool        isBuyerMaker;
};

struct KlineEvent {
    std::string symbol;
    Kline       kline;
    std::string interval;   // "1m", "5m", ...
};

struct MarkPriceEvent {
    std::string symbol;
    double      markPrice, indexPrice, estimatedSettlePrice;
    double      fundingRate;
    int64_t     nextFundingTime, time;
};

struct BookTickerEvent {
    std::string symbol;
    double      bidPrice, bidQty;
    double      askPrice, askQty;
    int64_t     transactTime;
};

struct DepthEvent {
    std::string                           symbol;
    int64_t                               firstUpdateId, finalUpdateId;
    int64_t                               prevFinalUpdateId;  // dùng để ghép incremental update
    std::vector<std::pair<double,double>> bids;   // {price, qty} — qty=0 nghĩa là xoá level
    std::vector<std::pair<double,double>> asks;
};

struct LiquidationEvent {
    std::string symbol;
    OrderSide   side;
    OrderType   type;
    TimeInForce timeInForce;
    std::string status;
    double      price, origQty, lastFilledQty, avgPrice, cumFilledQty;
    int64_t     time;
};

struct CompositeIndexEvent {
    std::string symbol;
    double      price;
    int64_t     time;
    struct Component {
        std::string baseAsset, quoteAsset;
        double      weightInQuantity, weightInPercentage, indexPrice;
    };
    std::vector<Component> components;
};

// Variant gộp tất cả market event types
using MarketEvent = std::variant<
    AggTradeEvent,
    KlineEvent,
    MarkPriceEvent,
    BookTickerEvent,
    DepthEvent,
    LiquidationEvent,
    CompositeIndexEvent
>;
```

#### User Data Events

```cpp
struct OrderUpdateEvent {
    std::string  symbol, clientOrderId, originalClientOrderId;
    int64_t      orderId;
    OrderSide    side;
    OrderType    type;
    PositionSide positionSide;
    TimeInForce  timeInForce;
    std::string  executionType;    // NEW | CANCELED | CALCULATED | EXPIRED | TRADE
    std::string  orderStatus;
    double       originalQty, originalPrice, avgPrice;
    double       lastFilledQty, lastFilledPrice;
    double       accumulatedFilledQty;
    double       realizedPnl, commission;
    std::string  commissionAsset;
    bool         isReduceOnly, closePosition;
    double       stopPrice, activationPrice, priceRate;
    WorkingType  workingType;
    int64_t      orderTime, tradeTime;
};

struct AccountUpdateEvent {
    std::string                         eventReason;   // DEPOSIT | WITHDRAW | ORDER | FUNDING_FEE | ...
    std::vector<Balance>                balances;
    std::vector<Position>               positions;
    int64_t                             time;
};

struct MarginCallEvent {
    std::vector<Position> positions;   // các vị thế gần bị thanh lý
    int64_t               time;
};

using UserDataEvent = std::variant<
    OrderUpdateEvent,
    AccountUpdateEvent,
    MarginCallEvent
>;
```

---

### 6.7 Transport — HttpSession (`src/transport/http_session.h`)

Quản lý một persistent HTTPS connection đến Binance REST host. Tự reconnect khi server đóng kết nối (sau ~15s idle).

```cpp
class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    using Result = std::expected<std::string, BinanceError>;  // raw JSON body

    HttpSession(asio::io_context& ioc, ssl::context& ssl, std::string host);

    asio::awaitable<Result> get (std::string_view path,
                                  std::string_view query,
                                  std::string_view apiKey = "");

    asio::awaitable<Result> post(std::string_view path,
                                  std::string_view body,
                                  std::string_view apiKey);

    asio::awaitable<Result> put (std::string_view path,
                                  std::string_view body,
                                  std::string_view apiKey);

    asio::awaitable<Result> del (std::string_view path,
                                  std::string_view query,
                                  std::string_view apiKey);

    // Header X-MBX-USED-WEIGHT-1M từ response gần nhất
    int lastUsedWeight() const { return m_lastUsedWeight; }

private:
    asio::awaitable<void>   ensureConnected();
    asio::awaitable<Result> execute(beast::http::request<beast::http::string_body> req);

    asio::io_context& m_ioc;
    ssl::context&     m_ssl;
    std::string       m_host;
    beast::ssl_stream<beast::tcp_stream> m_stream;
    bool              m_connected{false};
    std::atomic<int>  m_lastUsedWeight{0};
};
```

---

### 6.8 Transport — WsSession (`src/transport/ws_session.h`)

```cpp
using WsMessageCb  = std::function<void(boost::system::error_code, std::string_view)>;
using WsSimpleCb   = std::function<void()>;

struct ReconnectConfig {
    std::chrono::milliseconds initialBackoff  {1'000};
    std::chrono::milliseconds maxBackoff      {30'000};
    double                    backoffMultiplier{2.0};
};

class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    WsSession(asio::io_context& ioc, ssl::context& ssl,
              std::string host, ReconnectConfig cfg = {});

    // Bắt đầu kết nối và đọc messages
    void start(std::string       path,
               WsMessageCb       onMessage,
               WsSimpleCb        onDisconnect = {},
               WsSimpleCb        onReconnect  = {});

    void stop();

    // Gửi JSON command (subscribe/unsubscribe)
    void send(std::string message);

private:
    asio::awaitable<void> connectLoop();    // connect + read loop + reconnect
    asio::awaitable<void> doConnect();
    asio::awaitable<void> readLoop();
    asio::awaitable<void> keepaliveLoop(); // pong + 24h TTL reconnect

    asio::io_context& m_ioc;
    ssl::context&     m_ssl;
    std::string       m_host, m_path;
    beast::websocket::stream<
        beast::ssl_stream<beast::tcp_stream>> m_ws;

    WsMessageCb       m_onMessage;
    WsSimpleCb        m_onDisconnect, m_onReconnect;
    ReconnectConfig   m_reconnectCfg;
    std::atomic<bool> m_stopped{false};
    std::chrono::steady_clock::time_point m_connectedAt;
};
```

**Keepalive & 24h reconnect logic:**
1. Mỗi khi nhận ping frame từ server → trả pong ngay
2. Sau 23h50m kể từ khi connect → chủ động disconnect + reconnect (tránh bị server cắt sau 24h)
3. Khi reconnect: gọi `onDisconnect` → chờ backoff → kết nối lại → `onReconnect`
4. Backoff: 1s → 2s → 4s → ... → tối đa 30s

---

### 6.9 Signer (`src/rest/signer.h`)

```cpp
enum class SigningMethod { HMAC_SHA256, Ed25519 };

class Signer {
public:
    explicit Signer(std::string secretKey, SigningMethod method = SigningMethod::HMAC_SHA256);

    // Tính HMAC-SHA256 hoặc Ed25519 hex digest của payload
    std::string sign(std::string_view payload) const;

    // Thêm timestamp (ms) + signature vào cuối query string / body
    // Input:  "symbol=BTCUSDT&side=BUY"
    // Output: "symbol=BTCUSDT&side=BUY&timestamp=1234567890123&signature=abc..."
    std::string addSignature(std::string_view params) const;

    SigningMethod method() const { return m_method; }

private:
    std::string   m_secretKey;
    SigningMethod m_method;

    static int64_t nowMs();
};
```

**Signing Methods:**
- **HMAC-SHA256** (default): Sử dụng `HMAC(EVP_sha256(), ...)` từ OpenSSL. Kompatibel với tất cả Binance endpoints từ lâu.
- **Ed25519**: Sử dụng `EVP_PKEY` và `EVP_DigestSign()` từ OpenSSL 1.1.1+. Bảo mật cao hơn, hỗ trợ từ Binance 2024. `secretKey` là private key hex (64 ký tự).

Signature phải là parameter **cuối cùng** theo yêu cầu của Binance. Tự động chọn header `X-BAPI-SIGN-TYPE` trong RestClient dựa trên signing method.

---

### 6.10 RateLimiter (`src/rest/rate_limiter.h`)

```cpp
class RateLimiter {
public:
    struct Limits {
        int requestWeightPerMinute = 2400;   // Binance Futures default
        int ordersPerMinute        = 1200;
    };

    explicit RateLimiter(Limits limits = {});

    // Gọi sau mỗi HTTP response để cập nhật từ response headers
    void update(int usedWeight, int usedOrders);

    // true nếu đang dùng >= 80% quota
    bool isNearLimit() const;

    // Nếu đã đạt limit, block coroutine đến khi window reset (60s)
    // Dùng asio::this_coro::executor nội bộ — không cần truyền io_context
    asio::awaitable<void> waitIfNeeded();

private:
    Limits           m_limits;
    std::atomic<int> m_usedWeight{0};
    std::atomic<int> m_usedOrders{0};
    std::atomic<std::chrono::steady_clock::time_point> m_windowStart;
};
```

---

### 6.11 RestClient (`src/rest/rest_client.h`)

```cpp
template<typename T>
using Result = std::expected<T, BinanceError>;

class RestClient {
public:
    RestClient(asio::io_context& ioc, ssl::context& ssl, ContextConfig cfg);

    // ── Market Data (public) ─────────────────────────────────────────────
    asio::awaitable<Result<bool>>
        ping();

    asio::awaitable<Result<int64_t>>
        serverTime();

    asio::awaitable<Result<std::vector<ExchangeSymbol>>>
        exchangeInfo();

    asio::awaitable<Result<OrderBook>>
        orderBook(std::string symbol, int limit = 20);

    asio::awaitable<Result<std::vector<Kline>>>
        klines(std::string symbol, std::string interval,
               int limit = 500,
               std::optional<int64_t> startTime = {},
               std::optional<int64_t> endTime   = {});

    asio::awaitable<Result<MarkPrice>>
        markPrice(std::string symbol);

    asio::awaitable<Result<std::vector<MarkPrice>>>
        allMarkPrices();

    asio::awaitable<Result<double>>
        fundingRate(std::string symbol);

    asio::awaitable<Result<Ticker24h>>
        ticker24h(std::string symbol);

    asio::awaitable<Result<double>>
        bestBidPrice(std::string symbol);

    // ── Account (USER_DATA) ──────────────────────────────────────────────
    asio::awaitable<Result<FuturesAccount>>
        account();

    asio::awaitable<Result<std::vector<Balance>>>
        balance();

    asio::awaitable<Result<std::vector<Position>>>
        positions(std::optional<std::string> symbol = {});

    asio::awaitable<Result<LeverageResult>>
        setLeverage(std::string symbol, int leverage);

    asio::awaitable<Result<void>>
        setMarginType(std::string symbol,
                      std::string marginType);   // "ISOLATED" | "CROSSED"

    // ── Trading (TRADE) ──────────────────────────────────────────────────
    asio::awaitable<Result<Order>>
        newOrder(OrderRequest req);

    asio::awaitable<Result<Order>>
        cancelOrder(std::string symbol, int64_t orderId);

    asio::awaitable<Result<void>>
        cancelAllOrders(std::string symbol);

    asio::awaitable<Result<Order>>
        queryOrder(std::string symbol, int64_t orderId);

    asio::awaitable<Result<std::vector<Order>>>
        openOrders(std::optional<std::string> symbol = {});

    asio::awaitable<Result<std::vector<Order>>>
        allOrders(std::string symbol,
                  std::optional<int64_t> startTime = {},
                  std::optional<int64_t> endTime   = {},
                  int limit = 500);

    asio::awaitable<Result<BatchOrderResult>>
        batchOrders(std::vector<OrderRequest> reqs);

    // ── User Data Stream ─────────────────────────────────────────────────
    asio::awaitable<Result<std::string>>
        createListenKey();

    asio::awaitable<Result<void>>
        keepAliveListenKey(std::string listenKey);

    asio::awaitable<Result<void>>
        deleteListenKey(std::string listenKey);

private:
    std::shared_ptr<HttpSession> m_session;
    Signer                       m_signer;
    RateLimiter                  m_rateLimiter;
    ContextConfig                m_cfg;

    // Internal helpers — ký, gửi request, parse với simdjson, cập nhật rate limiter
    template<typename T, typename Parser>
    asio::awaitable<Result<T>>
    publicGet(std::string_view path, std::string query, Parser&& parser);

    template<typename T, typename Parser>
    asio::awaitable<Result<T>>
    signedGet(std::string_view path, std::string params, Parser&& parser);

    template<typename T, typename Parser>
    asio::awaitable<Result<T>>
    signedPost(std::string_view path, std::string params, Parser&& parser);

    template<typename T, typename Parser>
    asio::awaitable<Result<T>>
    signedDelete(std::string_view path, std::string params, Parser&& parser);

    // Expose raw document cho advanced caller muốn custom-parse JSON
    // Trả về: { parsed_value, raw_document } hoặc error
    // Lưu ý: document lifetime bị ràng buộc bởi parser instance
    template<typename T>
    Result<T> parseResponse(std::string_view body,
                            std::function<T(simdjson::ondemand::document&)> parser);

    // Advanced: Trả về raw simdjson::ondemand::document cho caller tự parse
    // Tự caller quản lý document lifetime
    // Ví dụ: auto [doc, err] = rest.rawParse(jsonBody);
    using RawParseResult = std::expected<
        std::pair<simdjson::ondemand::document, std::string_view>,
        BinanceError
    >;
    RawParseResult rawParse(std::string_view body);
};
```

---

### 6.12 WsClient (`src/ws/ws_client.h`)

```cpp
using MarketEventCb = std::function<void(boost::system::error_code, MarketEvent)>;

class WsClient {
public:
    WsClient(asio::io_context& ioc, ssl::context& ssl, ContextConfig cfg);

    // Đăng ký stream — accumulate trước khi gọi connect()
    void subscribeAggTrade     (std::string symbol, MarketEventCb cb);
    void subscribeKline        (std::string symbol, std::string interval, MarketEventCb cb);
    void subscribeMarkPrice    (std::string symbol, MarketEventCb cb);
    void subscribeBookTicker   (std::string symbol, MarketEventCb cb);
    void subscribeDepth        (std::string symbol, int levels,      // 5 | 10 | 20
                                std::string updateSpeed,             // "100ms" | "250ms" | "500ms"
                                MarketEventCb cb);
    void subscribeLiquidation  (std::string symbol, MarketEventCb cb);
    void subscribeCompositeIndex(std::string symbol, MarketEventCb cb);

    // Huỷ đăng ký — gửi UNSUBSCRIBE command nếu đang kết nối
    void unsubscribe   (std::string streamName);
    void unsubscribeAll();

    void setOnDisconnect(std::function<void()> cb);
    void setOnReconnect (std::function<void()> cb);

    // Kết nối với tất cả streams đã đăng ký dưới dạng combined stream
    void connect();
    void disconnect();

private:
    // Build: "/stream?streams=btcusdt@aggTrade/btcusdt@kline_1m/..."
    std::string buildStreamPath() const;

    void onRawMessage(boost::system::error_code ec, std::string_view raw);

    // simdjson parse raw message → MarketEvent, route đến đúng callback
    void dispatchEvent(simdjson::ondemand::document& doc);

    asio::io_context& m_ioc;
    ssl::context&     m_ssl;
    ContextConfig     m_cfg;

    std::shared_ptr<WsSession>                 m_session;
    std::map<std::string, MarketEventCb>       m_subscriptions;  // streamName → callback
    std::function<void()>                      m_onDisconnect, m_onReconnect;
};
```

**Combined stream format:** Binance hỗ trợ ghép tối đa 1024 streams trên 1 connection:
```
wss://fstream.binance.com/stream?streams=btcusdt@aggTrade/btcusdt@kline_1m/btcusdt@markPrice
```
Response từ combined stream được wrap:
```json
{ "stream": "btcusdt@aggTrade", "data": { ... } }
```
`dispatchEvent` đọc `"stream"` field → tìm callback trong `m_subscriptions` → parse `"data"` thành typed event.

**Sau reconnect:** `WsSession` gọi `onReconnect`, lúc đó `WsClient` gọi lại `connect()` với đúng danh sách stream tích luỹ trong `m_subscriptions`.

---

### 6.13 UserDataStream (`src/ws/user_data_stream.h`)

```cpp
using UserDataCb = std::function<void(boost::system::error_code, UserDataEvent)>;

class UserDataStream {
public:
    // Tự tạo RestClient nội bộ — caller không cần truyền RestClient vào
    UserDataStream(asio::io_context& ioc, ssl::context& ssl, ContextConfig cfg);

    // Tạo listen key → mở WS → bắt đầu keepalive loop
    void start(UserDataCb cb);

    // Huỷ listen key → đóng WS
    void stop();

    void setOnDisconnect(std::function<void()> cb);
    void setOnReconnect (std::function<void()> cb);

private:
    asio::awaitable<void> keepaliveLoop();    // PUT /fapi/v1/listenKey mỗi 30 phút
    void onRawMessage(boost::system::error_code ec, std::string_view raw);
    void onReconnect();   // tạo listen key mới, kết nối lại WS

    asio::io_context&          m_ioc;
    ssl::context&              m_ssl;
    RestClient                 m_rest;   // owned internally
    ContextConfig              m_cfg;
    std::string                m_listenKey;
    std::shared_ptr<WsSession> m_session;
    UserDataCb                 m_callback;
    asio::steady_timer         m_keepaliveTimer;
};
```

**Listen key lifecycle:**
| Action | Endpoint | Thời điểm |
|---|---|---|
| Tạo | `POST /fapi/v1/listenKey` | Khi `start()` |
| Keepalive | `PUT /fapi/v1/listenKey` | Mỗi 30 phút |
| Xoá | `DELETE /fapi/v1/listenKey` | Khi `stop()` |
| Tạo lại | `POST /fapi/v1/listenKey` | Sau mỗi WS reconnect |

**Lưu ý:** Listen key có TTL 60 phút nếu không keepalive. Keepalive mỗi 30 phút là an toàn.

---

## 7. Flow Sử Dụng Điển Hình

### 7.1 Setup và place lệnh futures

```cpp
int main() {
    BinanceContext ctx({
        .apiKey    = "YOUR_API_KEY",
        .secretKey = "YOUR_SECRET",
        .testnet   = true,
    });

    asio::co_spawn(ctx.ioc(), [&]() -> asio::awaitable<void> {
        auto rest = ctx.makeRestClient();

        // Kiểm tra kết nối
        co_await rest.ping();

        // Set 10x leverage
        auto lev = co_await rest.setLeverage("BTCUSDT", 10);
        if (!lev) throw std::runtime_error(lev.error().message);

        // Đặt lệnh Market Buy
        auto order = co_await rest.newOrder({
            .symbol   = "BTCUSDT",
            .side     = OrderSide::Buy,
            .type     = OrderType::Market,
            .quantity = 0.01,
        });

        if (order) {
            std::cout << "Filled @ " << order->avgPrice
                      << ", qty: " << order->executedQty << "\n";
        } else {
            std::cerr << "Error [" << order.error().code << "]: "
                      << order.error().message << "\n";
        }

    }, asio::detached);

    ctx.ioc().run();   // block until all work done
}
```

### 7.2 Subscribe market streams

```cpp
auto ws = ctx.makeWsClient();

ws.setOnDisconnect([] { std::cerr << "[WS] disconnected\n"; });
ws.setOnReconnect ([] { std::cerr << "[WS] reconnected\n"; });

ws.subscribeAggTrade("btcusdt", [](auto ec, auto event) {
    if (ec) return;
    auto& t = std::get<AggTradeEvent>(event);
    std::printf("Trade: %.2f x %.4f\n", t.price, t.qty);
});

ws.subscribeMarkPrice("btcusdt", [](auto ec, auto event) {
    auto& mp = std::get<MarkPriceEvent>(event);
    std::printf("Mark: %.2f  Funding: %.4f%%\n",
                mp.markPrice, mp.fundingRate * 100);
});

ws.subscribeDepth("btcusdt", 20, "100ms", [](auto ec, auto event) {
    auto& d = std::get<DepthEvent>(event);
    // process incremental order book update
});

ws.connect();  // single combined stream connection
```

### 7.3 User Data Stream — nhận order fills

```cpp
// UserDataStream tự quản lý RestClient nội bộ để tạo/keepalive listen key
auto uds = ctx.makeUserDataStream();

uds.start([](auto ec, auto event) {
    if (ec) return;

    if (auto* oe = std::get_if<OrderUpdateEvent>(&event)) {
        if (oe->executionType == "TRADE") {
            std::printf("FILL: %s %s %.4f @ %.2f  PnL=%.4f\n",
                oe->symbol.c_str(),
                oe->side == OrderSide::Buy ? "BUY" : "SELL",
                oe->lastFilledQty, oe->lastFilledPrice,
                oe->realizedPnl);
        }
    } else if (auto* ae = std::get_if<AccountUpdateEvent>(&event)) {
        for (auto& b : ae->balances) {
            std::printf("Balance %s: %.4f\n", b.asset.c_str(), b.walletBalance);
        }
    }
});
```

---

## 8. Error Handling Chi Tiết

### REST — mapping lỗi

| Tình huống | `ErrorCategory` | `code` |
|---|---|---|
| Binance trả `{"code":-1121,"msg":"Invalid symbol"}` | `Api` | `-1121` |
| HTTP 429 (rate limit) | `RateLimit` | `429` |
| HTTP 418 (IP ban) | `RateLimit` | `418` |
| Signature sai / key thiếu | `Auth` | `-2015` / `-2014` |
| simdjson không parse được | `Parse` | `0` |
| TLS handshake fail | `Network` | Boost error value |
| DNS resolve fail | `Network` | Boost error value |

### WebSocket — error_code trong callback

| Tình huống | `error_code` |
|---|---|
| Message hợp lệ | `{}` (zero / no error) |
| Connection bị drop (trước khi reconnect thành công) | `asio::error::connection_reset` |
| Parse error | custom error code |

Trong callback, luôn kiểm tra `ec` trước khi truy cập `event`:
```cpp
ws.subscribeAggTrade("btcusdt", [](auto ec, auto event) {
    if (ec) {
        std::cerr << "WS error: " << ec.message() << "\n";
        return;
    }
    // safe to use event
});
```

---

## 9. Testing Strategy

| File | Nội dung |
|---|---|
| `test_signing.cpp` | HMAC-SHA256 vectors: verify signature output với input biết trước |
| `test_types.cpp` | Parse JSON string → typed struct với simdjson cho từng data type |
| `test_rest_client.cpp` | Mock HTTP server bằng Boost.Beast embedded, kiểm tra: request URL format, headers (`X-MBX-APIKEY`), signed params, error mapping, rate limit header parsing |
| `test_ws_client.cpp` | Mock WebSocket server, kiểm tra: subscribe/unsubscribe, combined stream path building, event dispatch, reconnect callback sequence |

---

## 10. Thay Đổi CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(binance_trading_bot VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# ── Boost (Beast + ASIO + System) ──────────────────────────────────────
FetchContent_Declare(Boost
    URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-cmake.tar.xz
    # URL_HASH: điền vào lúc implementation (lấy từ trang release chính thức của Boost)
)
set(BOOST_INCLUDE_LIBRARIES beast asio system)
FetchContent_MakeAvailable(Boost)

# ── simdjson ───────────────────────────────────────────────────────────
FetchContent_Declare(simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        v3.10.0
)
FetchContent_MakeAvailable(simdjson)

# ── OpenSSL (system) ──────────────────────────────────────────────────
find_package(OpenSSL REQUIRED)

# ── GoogleTest ────────────────────────────────────────────────────────
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
FetchContent_MakeAvailable(googletest)

# ── Library sources ──────────────────────────────────────────────────
set(LIB_SOURCES
    src/context.cpp
    src/transport/http_session.cpp
    src/transport/ws_session.cpp
    src/rest/rest_client.cpp
    src/rest/signer.cpp
    src/rest/rate_limiter.cpp
    src/ws/ws_client.cpp
    src/ws/user_data_stream.cpp
)

add_library(binance_futures STATIC ${LIB_SOURCES})
target_include_directories(binance_futures PUBLIC src)
target_link_libraries(binance_futures PUBLIC
    Boost::beast
    Boost::asio
    Boost::system
    simdjson
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)
target_compile_features(binance_futures PUBLIC cxx_std_23)

# ── Executable ───────────────────────────────────────────────────────
add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE binance_futures)

# ── Tests ────────────────────────────────────────────────────────────
enable_testing()
file(GLOB TEST_SOURCES tests/*.cpp)
add_executable(${PROJECT_NAME}_tests ${TEST_SOURCES})
target_link_libraries(${PROJECT_NAME}_tests PRIVATE
    binance_futures
    GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(${PROJECT_NAME}_tests)
```

---

## 11. Những Điểm Thiết Kế Lại So Với dgrr/binance-futures-sdk

| Vấn đề trong dgrr | Giải pháp trong design này |
|---|---|
| Single-threaded, WS messages không được queue nội bộ | `io_context` thread pool configurable, không có global queue — mỗi coroutine tự await |
| HTTP connection tự close sau 15s, phải gọi `connect()` thủ công | `HttpSession::ensureConnected()` tự reconnect minh bạch trước mỗi request |
| Không có rate limit tracking | `RateLimiter` đọc `X-MBX-USED-WEIGHT-1M` header sau mỗi response |
| Không có User Data Stream | `UserDataStream` riêng với listen key lifecycle đầy đủ |
| Monolithic class gộp REST + WS | 4 module tách biệt với interface rõ ràng |
| Không notify reconnect | `onDisconnect`/`onReconnect` trên cả `WsClient` và `UserDataStream` |
| Không có error type rõ ràng | `BinanceError` với `ErrorCategory` và `std::expected` |

---

## 12. Quyết Định Thiết Kế Cuối Cùng

✅ **Ed25519 signing:** Hỗ trợ cả HMAC-SHA256 (default) và Ed25519. Định nghĩa `enum class SigningMethod { HMAC_SHA256, Ed25519 }` trong `Signer` class. Caller chọn via `ContextConfig::signingMethod` hoặc signer constructor. Signer tự chọn thuật toán signature dựa trên method này. Binance tự động nhận dạng signing method từ secret key format.

✅ **threadPoolSize:** Default = **2** cho production (đủ cho concurrent HTTP + WS event processing mà không quá heavy). Tránh head-of-line blocking với threadPoolSize=1. Caller có thể tăng lên 3–4 cho high-throughput scenarios.

✅ **Raw simdjson exposure:** Có expose `rawParse()` method trên `RestClient` để advanced caller tự parse complex nested JSON hoặc custom fields không được SDK cover. Safe — document lifetime tự caller quản lý trong scope của parser instance, không có hidden reference issues. Caller sử dụng khi cần flexibility vượt quá typed struct API.
