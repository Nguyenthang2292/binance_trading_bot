#include "rest/rest_client.h"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <simdjson.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace asio = boost::asio;

namespace {

std::string restHost(bool testnet) {
    return testnet ? "demo-fapi.binance.com" : "fapi.binance.com";
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

std::string boolParam(bool value) {
    return value ? "true" : "false";
}

std::string sideToString(OrderSide side) {
    return side == OrderSide::Buy ? "BUY" : "SELL";
}

std::string typeToString(OrderType type) {
    switch (type) {
        case OrderType::Limit: return "LIMIT";
        case OrderType::Market: return "MARKET";
        case OrderType::Stop: return "STOP";
        case OrderType::StopMarket: return "STOP_MARKET";
        case OrderType::TakeProfit: return "TAKE_PROFIT";
        case OrderType::TakeProfitMarket: return "TAKE_PROFIT_MARKET";
        case OrderType::TrailingStopMarket: return "TRAILING_STOP_MARKET";
    }
    return "MARKET";
}

std::string tifToString(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::GTX: return "GTX";
    }
    return "GTC";
}

std::string positionSideToString(PositionSide side) {
    switch (side) {
        case PositionSide::Both: return "BOTH";
        case PositionSide::Long: return "LONG";
        case PositionSide::Short: return "SHORT";
    }
    return "BOTH";
}

std::string workingTypeToString(WorkingType type) {
    return type == WorkingType::MarkPrice ? "MARK_PRICE" : "CONTRACT_PRICE";
}

OrderSide parseSide(std::string_view value) {
    return value == "SELL" ? OrderSide::Sell : OrderSide::Buy;
}

OrderType parseOrderType(std::string_view value) {
    if (value == "LIMIT") return OrderType::Limit;
    if (value == "STOP") return OrderType::Stop;
    if (value == "STOP_MARKET") return OrderType::StopMarket;
    if (value == "TAKE_PROFIT") return OrderType::TakeProfit;
    if (value == "TAKE_PROFIT_MARKET") return OrderType::TakeProfitMarket;
    if (value == "TRAILING_STOP_MARKET") return OrderType::TrailingStopMarket;
    return OrderType::Market;
}

TimeInForce parseTimeInForce(std::string_view value) {
    if (value == "IOC") return TimeInForce::IOC;
    if (value == "FOK") return TimeInForce::FOK;
    if (value == "GTX") return TimeInForce::GTX;
    return TimeInForce::GTC;
}

PositionSide parsePositionSide(std::string_view value) {
    if (value == "LONG") return PositionSide::Long;
    if (value == "SHORT") return PositionSide::Short;
    return PositionSide::Both;
}

WorkingType parseWorkingType(std::string_view value) {
    return value == "MARK_PRICE" ? WorkingType::MarkPrice : WorkingType::ContractPrice;
}

std::string urlEncode(std::string_view input);

std::string query(std::initializer_list<std::pair<std::string, std::string>> params) {
    std::string out;
    for (const auto& [k, v] : params) {
        if (v.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += '&';
        }
        out += urlEncode(k);
        out += '=';
        out += urlEncode(v);
    }
    return out;
}

bool shouldRetryPublicGet(const BinanceError& err) {
    if (err.code == 503) {
        return true;
    }
    return err.category == ErrorCategory::RateLimit && err.code == 429;
}

std::chrono::milliseconds retryDelay(const BinanceError& err, int attempt) {
    const int baseMs = (err.category == ErrorCategory::RateLimit) ? 1000 : 500;
    const int factor = 1 << std::max(0, attempt);
    return std::chrono::milliseconds(baseMs * factor);
}

std::string urlEncode(std::string_view input) {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
            out << std::setfill(' ');
        }
    }
    return out.str();
}

void appendParam(std::string& out, std::string_view key, std::string_view value) {
    if (value.empty()) {
        return;
    }
    if (!out.empty()) {
        out.push_back('&');
    }
    out.append(urlEncode(key));
    out.push_back('=');
    out.append(urlEncode(value));
}

std::string jsonEscape(std::string_view input) {
    std::ostringstream out;
    for (unsigned char c : input) {
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u"
                        << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
                        << static_cast<int>(c)
                        << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    return out.str();
}

void appendJsonStringField(std::ostringstream& json, std::string_view key, std::string_view value) {
    json << ",\"" << jsonEscape(key) << "\":\"" << jsonEscape(value) << "\"";
}

std::string_view asString(simdjson::ondemand::value value) {
    auto res = value.get_string();
    if (res.error()) {
        return {};
    }
    return res.value();
}

double toDouble(simdjson::ondemand::value value, double fallback = 0.0) {
    auto d = value.get_double();
    if (!d.error()) {
        return d.value();
    }
    auto i = value.get_int64();
    if (!i.error()) {
        return static_cast<double>(i.value());
    }
    auto s = asString(value);
    if (!s.empty()) {
        try {
            return std::stod(std::string(s));
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string stringField(simdjson::ondemand::object& object, std::string_view field, std::string fallback = {}) {
    auto value = object.find_field_unordered(field);
    if (value.error()) {
        return fallback;
    }
    auto s = asString(value.value());
    return s.empty() ? fallback : std::string(s);
}

int64_t intField(simdjson::ondemand::object& object, std::string_view field, int64_t fallback = 0) {
    auto value = object.find_field_unordered(field);
    if (value.error()) {
        return fallback;
    }
    auto i = value.value().get_int64();
    if (!i.error()) {
        return i.value();
    }
    auto d = value.value().get_double();
    if (!d.error()) {
        return static_cast<int64_t>(d.value());
    }
    return fallback;
}

bool boolField(simdjson::ondemand::object& object, std::string_view field, bool fallback = false) {
    auto value = object.find_field_unordered(field);
    if (value.error()) {
        return fallback;
    }
    auto b = value.value().get_bool();
    return b.error() ? fallback : b.value();
}

double doubleField(simdjson::ondemand::object& object, std::string_view field, double fallback = 0.0) {
    auto value = object.find_field_unordered(field);
    if (value.error()) {
        return fallback;
    }
    return toDouble(value.value(), fallback);
}

std::string decimalField(simdjson::ondemand::object& object, std::string_view field, std::string fallback = "0") {
    auto value = object.find_field_unordered(field);
    if (value.error()) {
        return fallback;
    }
    auto asStr = value.value().get_string();
    if (!asStr.error()) {
        return std::string(asStr.value());
    }
    auto asDouble = value.value().get_double();
    if (!asDouble.error()) {
        std::ostringstream out;
        out << std::setprecision(16) << asDouble.value();
        return out.str();
    }
    auto asInt = value.value().get_int64();
    if (!asInt.error()) {
        return std::to_string(asInt.value());
    }
    return fallback;
}

std::vector<std::pair<double, double>> parseLevels(simdjson::ondemand::array levels) {
    std::vector<std::pair<double, double>> out;
    for (auto row : levels) {
        auto arr = row.get_array();
        if (arr.error()) {
            continue;
        }
        int index = 0;
        double price = 0.0;
        double qty = 0.0;
        for (auto cell : arr.value()) {
            if (index == 0) {
                price = toDouble(cell.value());
            } else if (index == 1) {
                qty = toDouble(cell.value());
                break;
            }
            ++index;
        }
        if (index >= 1) {
            out.emplace_back(price, qty);
        }
    }
    return out;
}

Kline parseKlineArray(simdjson::ondemand::array row) {
    Kline k;
    size_t index = 0;
    for (auto cell : row) {
        auto value = cell.value();
        switch (index) {
            case 0: {
                auto t = value.get_int64();
                if (!t.error()) k.openTime = t.value();
                break;
            }
            case 1: k.open = toDouble(value); break;
            case 2: k.high = toDouble(value); break;
            case 3: k.low = toDouble(value); break;
            case 4: k.close = toDouble(value); break;
            case 5: k.volume = toDouble(value); break;
            case 6: {
                auto t = value.get_int64();
                if (!t.error()) k.closeTime = t.value();
                break;
            }
            case 7: k.quoteVolume = toDouble(value); k.quoteAssetVolume = k.quoteVolume; break;
            case 8: {
                int64_t trades = 0;
                auto t = value.get_int64();
                if (!t.error()) trades = t.value();
                k.tradeCount = static_cast<int32_t>(trades);
                k.numberOfTrades = k.tradeCount;
                break;
            }
            default: break;
        }
        ++index;
    }
    k.isClosed = true;
    return k;
}

Order parseOrder(simdjson::ondemand::object& doc) {
    Order order;
    order.symbol = stringField(doc, "symbol");
    order.clientOrderId = stringField(doc, "clientOrderId");
    order.orderId = intField(doc, "orderId");
    order.side = parseSide(stringField(doc, "side"));
    order.type = parseOrderType(stringField(doc, "type"));
    order.positionSide = parsePositionSide(stringField(doc, "positionSide"));
    order.timeInForce = parseTimeInForce(stringField(doc, "timeInForce"));
    order.status = stringField(doc, "status");
    order.price = decimalField(doc, "price");
    order.origQty = decimalField(doc, "origQty");
    order.executedQty = decimalField(doc, "executedQty");
    order.avgPrice = decimalField(doc, "avgPrice");
    order.cumQuote = decimalField(doc, "cumQuote");
    order.reduceOnly = boolField(doc, "reduceOnly");
    order.closePosition = boolField(doc, "closePosition");
    order.stopPrice = decimalField(doc, "stopPrice");
    order.activationPrice = decimalField(doc, "activatePrice");
    order.priceRate = decimalField(doc, "priceRate");
    order.workingType = parseWorkingType(stringField(doc, "workingType"));
    order.time = intField(doc, "time");
    order.updateTime = intField(doc, "updateTime");
    return order;
}

Balance parseBalance(simdjson::ondemand::object& doc) {
    Balance b;
    b.asset = stringField(doc, "asset");
    b.walletBalance = doubleField(doc, "walletBalance");
    b.crossWalletBalance = doubleField(doc, "crossWalletBalance");
    b.unrealizedProfit = doubleField(doc, "unrealizedProfit");
    b.marginBalance = doubleField(doc, "marginBalance");
    b.maintMargin = doubleField(doc, "maintMargin");
    b.initialMargin = doubleField(doc, "initialMargin");
    b.availableBalance = doubleField(doc, "availableBalance");
    b.maxWithdrawAmount = doubleField(doc, "maxWithdrawAmount");
    return b;
}

Position parsePosition(simdjson::ondemand::object& doc) {
    Position p;
    p.symbol = stringField(doc, "symbol");
    p.positionSide = parsePositionSide(stringField(doc, "positionSide"));
    p.positionAmt = doubleField(doc, "positionAmt");
    p.entryPrice = doubleField(doc, "entryPrice");
    p.breakEvenPrice = doubleField(doc, "breakEvenPrice");
    p.markPrice = doubleField(doc, "markPrice");
    p.unrealizedProfit = doubleField(doc, "unrealizedProfit");
    p.liquidationPrice = doubleField(doc, "liquidationPrice");
    p.leverage = static_cast<int>(intField(doc, "leverage"));
    p.marginType = stringField(doc, "marginType");
    p.isolatedMargin = doubleField(doc, "isolatedMargin");
    p.initialMargin = doubleField(doc, "initialMargin");
    p.maintMargin = doubleField(doc, "maintMargin");
    p.notional = doubleField(doc, "notional");
    return p;
}

} // namespace

RestClient::RestClient(asio::io_context& ioc, boost::asio::ssl::context& ssl, ContextConfig cfg)
    : RestClient(ioc, ssl, std::move(cfg), nullptr) {}

RestClient::RestClient(
    asio::io_context& ioc,
    boost::asio::ssl::context& ssl,
    ContextConfig cfg,
    std::shared_ptr<RateLimiter> sharedRateLimiter)
    : m_session(std::make_shared<HttpSession>(ioc, ssl, restHost(cfg.testnet), cfg.socks5Proxy)),
      m_signer(cfg.secretKey, cfg.signingMethod),
      m_rateLimiter(sharedRateLimiter ? std::move(sharedRateLimiter) : std::make_shared<RateLimiter>()),
      m_cfg(std::move(cfg)) {}

RestClient::RawParseResult RestClient::rawParse(std::string_view body) {
    std::scoped_lock lock(m_rawParseMutex);
    m_rawBuffer = simdjson::padded_string(body);
    auto docResult = m_rawParser.iterate(m_rawBuffer);
    if (docResult.error()) {
        return std::unexpected(BinanceError::fromParse(simdjson::error_message(docResult.error())));
    }
    auto doc = std::move(docResult).value();
    auto type = doc.type();
    if (type.error()) {
        return std::unexpected(BinanceError::fromParse(simdjson::error_message(type.error())));
    }
    return std::pair<simdjson::ondemand::document, std::string_view>{
        std::move(doc),
        std::string_view(m_rawBuffer.data(), m_rawBuffer.length())};
}

asio::awaitable<HttpSession::Result> RestClient::publicGet(
    std::string_view path,
    std::string q,
    RateLimiter::Cost cost) {
    constexpr int kMaxAttempts = 3;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        co_await m_rateLimiter->acquire(cost);
        auto result = co_await m_session->get(path, q);
        m_rateLimiter->updateFromHeaders(
            m_session->lastUsedWeight(),
            m_session->lastUsedOrders(),
            m_session->lastUsedOrders10s());
        if (result) {
            co_return result;
        }
        if (!shouldRetryPublicGet(result.error()) || attempt + 1 >= kMaxAttempts) {
            if (result.error().category == ErrorCategory::RateLimit && result.error().code == 429) {
                m_rateLimiter->penalize(retryDelay(result.error(), attempt));
            }
            co_return result;
        }
        const auto delay = retryDelay(result.error(), attempt);
        if (result.error().category == ErrorCategory::RateLimit && result.error().code == 429) {
            m_rateLimiter->penalize(delay);
        }
        auto executor = co_await asio::this_coro::executor;
        asio::steady_timer timer(executor);
        timer.expires_after(delay);
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return std::unexpected(BinanceError::fromApiResponse(-91002, "unexpected publicGet retry flow"));
}

asio::awaitable<HttpSession::Result> RestClient::signedGet(
    std::string_view path,
    std::string params,
    RateLimiter::Cost cost) {
    co_await m_rateLimiter->acquire(cost);
    auto result = co_await m_session->get(path, m_signer.addSignature(params), m_cfg.apiKey);
    m_rateLimiter->updateFromHeaders(
        m_session->lastUsedWeight(),
        m_session->lastUsedOrders(),
        m_session->lastUsedOrders10s());
    co_return result;
}

asio::awaitable<HttpSession::Result> RestClient::signedPost(
    std::string_view path,
    std::string params,
    RateLimiter::Cost cost) {
    co_await m_rateLimiter->acquire(cost);
    auto result = co_await m_session->post(path, m_signer.addSignature(params), m_cfg.apiKey);
    m_rateLimiter->updateFromHeaders(
        m_session->lastUsedWeight(),
        m_session->lastUsedOrders(),
        m_session->lastUsedOrders10s());
    co_return result;
}

asio::awaitable<HttpSession::Result> RestClient::signedPut(
    std::string_view path,
    std::string params,
    RateLimiter::Cost cost) {
    co_await m_rateLimiter->acquire(cost);
    auto result = co_await m_session->put(path, m_signer.addSignature(params), m_cfg.apiKey);
    m_rateLimiter->updateFromHeaders(
        m_session->lastUsedWeight(),
        m_session->lastUsedOrders(),
        m_session->lastUsedOrders10s());
    co_return result;
}

asio::awaitable<HttpSession::Result> RestClient::signedDelete(
    std::string_view path,
    std::string params,
    RateLimiter::Cost cost) {
    co_await m_rateLimiter->acquire(cost);
    auto result = co_await m_session->del(path, m_signer.addSignature(params), m_cfg.apiKey);
    m_rateLimiter->updateFromHeaders(
        m_session->lastUsedWeight(),
        m_session->lastUsedOrders(),
        m_session->lastUsedOrders10s());
    co_return result;
}

asio::awaitable<Result<bool>> RestClient::ping() {
    auto body = co_await publicGet("/fapi/v1/ping", {});
    if (!body) co_return std::unexpected(body.error());
    co_return true;
}

asio::awaitable<Result<int64_t>> RestClient::serverTime() {
    auto body = co_await publicGet("/fapi/v1/time", {});
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<int64_t>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return intField(object, "serverTime");
    });
}

asio::awaitable<Result<std::vector<ExchangeSymbol>>> RestClient::exchangeInfo() {
    auto body = co_await publicGet("/fapi/v1/exchangeInfo", {});
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<std::vector<ExchangeSymbol>>(*body, [](simdjson::ondemand::document& doc) {
        std::vector<ExchangeSymbol> symbols;
        auto object = doc.get_object().value();
        auto symbolsArray = object.find_field_unordered("symbols").get_array().value();
        for (auto itemValue : symbolsArray) {
            auto item = itemValue.get_object().value();
            ExchangeSymbol s;
            s.symbol = stringField(item, "symbol");
            s.baseAsset = stringField(item, "baseAsset");
            s.quoteAsset = stringField(item, "quoteAsset");
            s.contractType = stringField(item, "contractType");
            s.status = stringField(item, "status");
            s.pricePrecision = static_cast<int>(intField(item, "pricePrecision"));
            s.quantityPrecision = static_cast<int>(intField(item, "quantityPrecision"));
            s.baseAssetPrecision = static_cast<int>(intField(item, "baseAssetPrecision"));
            auto filters = item.find_field_unordered("filters").get_array().value();
            for (auto filterValue : filters) {
                auto filter = filterValue.get_object().value();
                const auto type = stringField(filter, "filterType");
                if (type == "PRICE_FILTER") s.tickSize = doubleField(filter, "tickSize");
                if (type == "LOT_SIZE" || type == "MARKET_LOT_SIZE") {
                    s.stepSize = doubleField(filter, "stepSize", s.stepSize);
                    s.minQty = doubleField(filter, "minQty", s.minQty);
                    s.maxQty = doubleField(filter, "maxQty", s.maxQty);
                }
                if (type == "MIN_NOTIONAL") s.minNotional = doubleField(filter, "notional");
            }
            symbols.push_back(std::move(s));
        }
        return symbols;
    });
}

asio::awaitable<Result<OrderBook>> RestClient::orderBook(std::string symbol, int limit) {
    auto body = co_await publicGet("/fapi/v1/depth", query({{"symbol", upper(symbol)}, {"limit", std::to_string(limit)}}));
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<OrderBook>(*body, [](simdjson::ondemand::document& doc) {
        OrderBook book;
        auto object = doc.get_object().value();
        book.lastUpdateId = intField(object, "lastUpdateId");
        book.bids = parseLevels(object.find_field_unordered("bids").get_array().value());
        book.asks = parseLevels(object.find_field_unordered("asks").get_array().value());
        return book;
    });
}

asio::awaitable<Result<std::vector<Kline>>> RestClient::klines(std::string symbol,
                                                              std::string interval,
                                                              int limit,
                                                              std::optional<int64_t> startTime,
                                                              std::optional<int64_t> endTime) {
    auto q = query({
        {"symbol", upper(symbol)},
        {"interval", interval},
        {"limit", std::to_string(limit)},
        {"startTime", startTime ? std::to_string(*startTime) : ""},
        {"endTime", endTime ? std::to_string(*endTime) : ""},
    });
    const int cost = RateLimiter::klineWeight(limit);
    auto body = co_await publicGet("/fapi/v1/klines", q, RateLimiter::Cost{.requestWeight = cost});
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<std::vector<Kline>>(*body, [](simdjson::ondemand::document& doc) {
        std::vector<Kline> out;
        auto rows = doc.get_array().value();
        for (auto rowValue : rows) {
            out.push_back(parseKlineArray(rowValue.get_array().value()));
        }
        return out;
    });
}

asio::awaitable<Result<MarkPrice>> RestClient::markPrice(std::string symbol) {
    auto body = co_await publicGet("/fapi/v1/premiumIndex", query({{"symbol", upper(symbol)}}));
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<MarkPrice>(*body, [](simdjson::ondemand::document& doc) {
        MarkPrice p;
        auto object = doc.get_object().value();
        p.symbol = stringField(object, "symbol");
        p.markPrice = doubleField(object, "markPrice");
        p.indexPrice = doubleField(object, "indexPrice");
        p.estimatedSettlePrice = doubleField(object, "estimatedSettlePrice");
        p.fundingRate = doubleField(object, "lastFundingRate");
        p.nextFundingTime = intField(object, "nextFundingTime");
        p.time = intField(object, "time");
        return p;
    });
}

asio::awaitable<Result<std::vector<MarkPrice>>> RestClient::allMarkPrices() {
    auto body = co_await publicGet("/fapi/v1/premiumIndex", {});
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<std::vector<MarkPrice>>(*body, [](simdjson::ondemand::document& doc) {
        std::vector<MarkPrice> out;
        auto array = doc.get_array().value();
        for (auto itemValue : array) {
            auto item = itemValue.get_object().value();
            MarkPrice p;
            p.symbol = stringField(item, "symbol");
            p.markPrice = doubleField(item, "markPrice");
            p.indexPrice = doubleField(item, "indexPrice");
            p.estimatedSettlePrice = doubleField(item, "estimatedSettlePrice");
            p.fundingRate = doubleField(item, "lastFundingRate");
            p.nextFundingTime = intField(item, "nextFundingTime");
            p.time = intField(item, "time");
            out.push_back(std::move(p));
        }
        return out;
    });
}

asio::awaitable<Result<double>> RestClient::fundingRate(std::string symbol) {
    auto body = co_await publicGet("/fapi/v1/fundingRate", query({{"symbol", upper(symbol)}, {"limit", "1"}}));
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<double>(*body, [](simdjson::ondemand::document& doc) {
        auto array = doc.get_array().value();
        for (auto itemValue : array) {
            auto item = itemValue.get_object().value();
            return doubleField(item, "fundingRate");
        }
        return 0.0;
    });
}

asio::awaitable<Result<Ticker24h>> RestClient::ticker24h(std::string symbol) {
    auto body = co_await publicGet("/fapi/v1/ticker/24hr", query({{"symbol", upper(symbol)}}));
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<Ticker24h>(*body, [](simdjson::ondemand::document& doc) {
        Ticker24h t;
        auto object = doc.get_object().value();
        t.symbol = stringField(object, "symbol");
        t.lastPrice = doubleField(object, "lastPrice");
        t.priceChange = doubleField(object, "priceChange");
        t.priceChangePercent = doubleField(object, "priceChangePercent");
        t.highPrice = doubleField(object, "highPrice");
        t.lowPrice = doubleField(object, "lowPrice");
        t.volume = doubleField(object, "volume");
        t.quoteVolume = doubleField(object, "quoteVolume");
        t.openTime = intField(object, "openTime");
        t.closeTime = intField(object, "closeTime");
        return t;
    });
}

asio::awaitable<Result<std::vector<Ticker24h>>> RestClient::allTicker24h() {
    auto body = co_await publicGet("/fapi/v1/ticker/24hr", {});
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<std::vector<Ticker24h>>(*body, [](simdjson::ondemand::document& doc) {
        std::vector<Ticker24h> out;
        auto array = doc.get_array().value();
        for (auto itemValue : array) {
            auto item = itemValue.get_object().value();
            Ticker24h t;
            t.symbol = stringField(item, "symbol");
            t.lastPrice = doubleField(item, "lastPrice");
            t.priceChange = doubleField(item, "priceChange");
            t.priceChangePercent = doubleField(item, "priceChangePercent");
            t.highPrice = doubleField(item, "highPrice");
            t.lowPrice = doubleField(item, "lowPrice");
            t.volume = doubleField(item, "volume");
            t.quoteVolume = doubleField(item, "quoteVolume");
            t.openTime = intField(item, "openTime");
            t.closeTime = intField(item, "closeTime");
            out.push_back(std::move(t));
        }
        return out;
    });
}

asio::awaitable<Result<double>> RestClient::bestBidPrice(std::string symbol) {
    auto body = co_await publicGet("/fapi/v1/ticker/bookTicker", query({{"symbol", upper(symbol)}}));
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<double>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return doubleField(object, "bidPrice");
    });
}

asio::awaitable<Result<FuturesAccount>> RestClient::account() {
    auto body = co_await signedGet("/fapi/v2/account", {});
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<FuturesAccount>(*body, [](simdjson::ondemand::document& doc) {
        FuturesAccount a;
        auto object = doc.get_object().value();
        a.feeTier = doubleField(object, "feeTier");
        a.canTrade = boolField(object, "canTrade");
        a.canDeposit = boolField(object, "canDeposit");
        a.canWithdraw = boolField(object, "canWithdraw");
        a.totalWalletBalance = doubleField(object, "totalWalletBalance");
        a.totalUnrealizedProfit = doubleField(object, "totalUnrealizedProfit");
        a.totalMarginBalance = doubleField(object, "totalMarginBalance");
        a.totalInitialMargin = doubleField(object, "totalInitialMargin");
        a.totalMaintMargin = doubleField(object, "totalMaintMargin");
        a.availableBalance = doubleField(object, "availableBalance");
        a.maxWithdrawAmount = doubleField(object, "maxWithdrawAmount");
        auto assets = object.find_field_unordered("assets").get_array().value();
        for (auto itemValue : assets) {
            auto item = itemValue.get_object().value();
            a.assets.push_back(parseBalance(item));
        }
        auto positions = object.find_field_unordered("positions").get_array().value();
        for (auto itemValue : positions) {
            auto item = itemValue.get_object().value();
            a.positions.push_back(parsePosition(item));
        }
        return a;
    });
}

asio::awaitable<Result<std::vector<Balance>>> RestClient::balance() {
    auto body = co_await signedGet("/fapi/v2/balance", {});
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<std::vector<Balance>>(*body, [](simdjson::ondemand::document& doc) {
        std::vector<Balance> out;
        auto array = doc.get_array().value();
        for (auto itemValue : array) {
            auto item = itemValue.get_object().value();
            out.push_back(parseBalance(item));
        }
        return out;
    });
}

asio::awaitable<Result<std::vector<Position>>> RestClient::positions(std::optional<std::string> symbol) {
    auto body = co_await signedGet("/fapi/v2/positionRisk", symbol ? query({{"symbol", upper(*symbol)}}) : "");
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<std::vector<Position>>(*body, [](simdjson::ondemand::document& doc) {
        std::vector<Position> out;
        auto array = doc.get_array().value();
        for (auto itemValue : array) {
            auto item = itemValue.get_object().value();
            out.push_back(parsePosition(item));
        }
        return out;
    });
}

asio::awaitable<Result<FuturesAccountConfig>> RestClient::accountConfig() {
    auto body = co_await signedGet("/fapi/v1/accountConfig", {});
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<FuturesAccountConfig>(*body, [](simdjson::ondemand::document& doc) {
        FuturesAccountConfig config;
        auto object = doc.get_object().value();
        config.canTrade = boolField(object, "canTrade");
        config.dualSidePosition = boolField(object, "dualSidePosition");
        config.multiAssetsMargin = boolField(object, "multiAssetsMargin");
        return config;
    });
}

asio::awaitable<Result<PositionModeStatus>> RestClient::positionMode() {
    auto body = co_await signedGet("/fapi/v1/positionSide/dual", {});
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<PositionModeStatus>(*body, [](simdjson::ondemand::document& doc) {
        PositionModeStatus mode;
        auto object = doc.get_object().value();
        mode.dualSidePosition = boolField(object, "dualSidePosition");
        return mode;
    });
}

asio::awaitable<Result<MultiAssetsModeStatus>> RestClient::multiAssetsMode() {
    auto body = co_await signedGet("/fapi/v1/multiAssetsMargin", {});
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<MultiAssetsModeStatus>(*body, [](simdjson::ondemand::document& doc) {
        MultiAssetsModeStatus mode;
        auto object = doc.get_object().value();
        mode.multiAssetsMargin = boolField(object, "multiAssetsMargin");
        return mode;
    });
}

asio::awaitable<Result<void>> RestClient::testOrder(OrderRequest req) {
    std::string params;
    appendParam(params, "symbol", upper(req.symbol));
    appendParam(params, "side", sideToString(req.side));
    appendParam(params, "type", typeToString(req.type));
    appendParam(params, "positionSide", positionSideToString(req.positionSide));
    appendParam(params, "quantity", req.quantity);
    appendParam(params, "price", req.price.value_or(""));
    appendParam(params, "stopPrice", req.stopPrice.value_or(""));
    appendParam(params, "activationPrice", req.activationPrice.value_or(""));
    appendParam(params, "callbackRate", req.callbackRate.value_or(""));
    appendParam(params,
                "timeInForce",
                req.timeInForce ? tifToString(*req.timeInForce) : (req.type == OrderType::Limit ? "GTC" : ""));
    appendParam(params, "reduceOnly", req.reduceOnly ? boolParam(*req.reduceOnly) : "");
    appendParam(params, "closePosition", req.closePosition ? boolParam(*req.closePosition) : "");
    appendParam(params, "workingType", req.workingType ? workingTypeToString(*req.workingType) : "");
    appendParam(params, "newClientOrderId", req.newClientOrderId.value_or(""));
    appendParam(params, "newOrderRespType", req.newOrderRespType.value_or(""));
    appendParam(params, "recvWindow", req.recvWindow ? std::to_string(*req.recvWindow) : "");
    for (const auto& [k, v] : req.extraParams) {
        appendParam(params, k, v);
    }
    auto body = co_await signedPost("/fapi/v1/order/test", params);
    if (!body) co_return std::unexpected(body.error());
    co_return Result<void>{};
}

asio::awaitable<Result<std::vector<SymbolLeverageBrackets>>> RestClient::leverageBrackets(
    std::optional<std::string> symbol) {
    auto body = co_await signedGet("/fapi/v1/leverageBracket", symbol ? query({{"symbol", upper(*symbol)}}) : "");
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<std::vector<SymbolLeverageBrackets>>(
        *body,
        [](simdjson::ondemand::document& doc) {
            std::vector<SymbolLeverageBrackets> out;
            auto array = doc.get_array().value();
            for (auto itemValue : array) {
                auto item = itemValue.get_object().value();
                SymbolLeverageBrackets entry;
                entry.symbol = stringField(item, "symbol");
                auto brackets = item.find_field_unordered("brackets").get_array().value();
                for (auto bracketValue : brackets) {
                    auto bracketObj = bracketValue.get_object().value();
                    LeverageBracketTier tier;
                    tier.bracket = static_cast<int>(intField(bracketObj, "bracket"));
                    tier.initialLeverage = static_cast<int>(intField(bracketObj, "initialLeverage"));
                    tier.notionalCap = doubleField(bracketObj, "notionalCap");
                    tier.notionalFloor = doubleField(bracketObj, "notionalFloor");
                    tier.maintMarginRatio = doubleField(bracketObj, "maintMarginRatio");
                    tier.cum = doubleField(bracketObj, "cum");
                    entry.brackets.push_back(tier);
                }
                out.push_back(std::move(entry));
            }
            return out;
        });
}

asio::awaitable<Result<LeverageResult>> RestClient::setLeverage(std::string symbol, int leverage) {
    auto body = co_await signedPost("/fapi/v1/leverage", query({{"symbol", upper(symbol)}, {"leverage", std::to_string(leverage)}}));
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<LeverageResult>(*body, [](simdjson::ondemand::document& doc) {
        LeverageResult r;
        auto object = doc.get_object().value();
        r.symbol = stringField(object, "symbol");
        r.leverage = static_cast<int>(intField(object, "leverage"));
        r.maxNotionalValue = doubleField(object, "maxNotionalValue");
        return r;
    });
}

asio::awaitable<Result<void>> RestClient::setMarginType(std::string symbol, std::string marginType) {
    auto body = co_await signedPost("/fapi/v1/marginType", query({{"symbol", upper(symbol)}, {"marginType", upper(marginType)}}));
    if (!body) co_return std::unexpected(body.error());
    co_return Result<void>{};
}

asio::awaitable<Result<Order>> RestClient::newOrder(OrderRequest req) {
    std::string params;
    appendParam(params, "symbol", upper(req.symbol));
    appendParam(params, "side", sideToString(req.side));
    appendParam(params, "type", typeToString(req.type));
    appendParam(params, "positionSide", positionSideToString(req.positionSide));
    appendParam(params, "quantity", req.quantity);
    appendParam(params, "price", req.price.value_or(""));
    appendParam(params, "stopPrice", req.stopPrice.value_or(""));
    appendParam(params, "activationPrice", req.activationPrice.value_or(""));
    appendParam(params, "callbackRate", req.callbackRate.value_or(""));
    appendParam(params,
                "timeInForce",
                req.timeInForce ? tifToString(*req.timeInForce) : (req.type == OrderType::Limit ? "GTC" : ""));
    appendParam(params, "reduceOnly", req.reduceOnly ? boolParam(*req.reduceOnly) : "");
    appendParam(params, "closePosition", req.closePosition ? boolParam(*req.closePosition) : "");
    appendParam(params, "workingType", req.workingType ? workingTypeToString(*req.workingType) : "");
    appendParam(params, "newClientOrderId", req.newClientOrderId.value_or(""));
    appendParam(params, "newOrderRespType", req.newOrderRespType.value_or(""));
    appendParam(params, "recvWindow", req.recvWindow ? std::to_string(*req.recvWindow) : "");
    for (const auto& [k, v] : req.extraParams) {
        appendParam(params, k, v);
    }
    auto body = co_await signedPost("/fapi/v1/order", params);
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseOrder(object);
    });
}

asio::awaitable<Result<Order>> RestClient::modifyOrder(OrderRequest req) {
    std::string params;
    appendParam(params, "symbol", upper(req.symbol));
    appendParam(params, "side", sideToString(req.side));
    appendParam(params, "quantity", req.quantity);
    appendParam(params, "price", req.price.value_or(""));
    if (req.orderId != 0) {
        appendParam(params, "orderId", std::to_string(req.orderId));
    }
    if (req.origClientOrderId) {
        appendParam(params, "origClientOrderId", *req.origClientOrderId);
    }
    appendParam(params, "recvWindow", req.recvWindow ? std::to_string(*req.recvWindow) : "");
    for (const auto& [k, v] : req.extraParams) {
        appendParam(params, k, v);
    }
    auto body = co_await signedPut("/fapi/v1/order", params);
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseOrder(object);
    });
}

asio::awaitable<Result<Order>> RestClient::cancelOrder(std::string symbol, int64_t orderId) {
    auto body = co_await signedDelete("/fapi/v1/order", query({{"symbol", upper(symbol)}, {"orderId", std::to_string(orderId)}}));
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseOrder(object);
    });
}

asio::awaitable<Result<Order>> RestClient::cancelOrderByClientOrderId(std::string symbol, std::string clientOrderId) {
    auto body = co_await signedDelete(
        "/fapi/v1/order",
        query({{"symbol", upper(symbol)}, {"origClientOrderId", clientOrderId}}));
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseOrder(object);
    });
}

asio::awaitable<Result<void>> RestClient::cancelAllOrders(std::string symbol) {
    auto body = co_await signedDelete("/fapi/v1/allOpenOrders", query({{"symbol", upper(symbol)}}));
    if (!body) co_return std::unexpected(body.error());
    co_return Result<void>{};
}

asio::awaitable<Result<Order>> RestClient::queryOrder(std::string symbol, int64_t orderId) {
    auto body = co_await signedGet("/fapi/v1/order", query({{"symbol", upper(symbol)}, {"orderId", std::to_string(orderId)}}));
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseOrder(object);
    });
}

asio::awaitable<Result<Order>> RestClient::queryOrderByClientOrderId(std::string symbol, std::string clientOrderId) {
    auto body = co_await signedGet(
        "/fapi/v1/order",
        query({{"symbol", upper(symbol)}, {"origClientOrderId", clientOrderId}}));
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseOrder(object);
    });
}

asio::awaitable<Result<std::vector<Order>>> RestClient::openOrders(std::optional<std::string> symbol) {
    auto body = co_await signedGet("/fapi/v1/openOrders", symbol ? query({{"symbol", upper(*symbol)}}) : "");
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<std::vector<Order>>(*body, [](simdjson::ondemand::document& doc) {
        std::vector<Order> out;
        auto array = doc.get_array().value();
        for (auto itemValue : array) {
            auto item = itemValue.get_object().value();
            out.push_back(parseOrder(item));
        }
        return out;
    });
}

asio::awaitable<Result<std::vector<Order>>> RestClient::allOrders(std::string symbol,
                                                                 std::optional<int64_t> startTime,
                                                                 std::optional<int64_t> endTime,
                                                                 int limit) {
    auto params = query({
        {"symbol", upper(symbol)},
        {"startTime", startTime ? std::to_string(*startTime) : ""},
        {"endTime", endTime ? std::to_string(*endTime) : ""},
        {"limit", std::to_string(limit)},
    });
    auto body = co_await signedGet("/fapi/v1/allOrders", params);
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<std::vector<Order>>(*body, [](simdjson::ondemand::document& doc) {
        std::vector<Order> out;
        auto array = doc.get_array().value();
        for (auto itemValue : array) {
            auto item = itemValue.get_object().value();
            out.push_back(parseOrder(item));
        }
        return out;
    });
}

asio::awaitable<Result<std::vector<UserTrade>>> RestClient::userTrades(std::string symbol,
                                                                   std::optional<int64_t> orderId,
                                                                   std::optional<int64_t> startTime,
                                                                   std::optional<int64_t> endTime,
                                                                   int limit) {
    auto params = query({
        {"symbol", upper(symbol)},
        {"orderId", orderId ? std::to_string(*orderId) : ""},
        {"startTime", startTime ? std::to_string(*startTime) : ""},
        {"endTime", endTime ? std::to_string(*endTime) : ""},
        {"limit", std::to_string(limit)},
    });
    auto body = co_await signedGet("/fapi/v1/userTrades", params);
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<std::vector<UserTrade>>(*body, [](simdjson::ondemand::document& doc) {
        std::vector<UserTrade> out;
        auto array = doc.get_array().value();
        for (auto itemValue : array) {
            auto item = itemValue.get_object().value();
            UserTrade t;
            t.symbol = stringField(item, "symbol");
            t.id = intField(item, "id");
            t.orderId = intField(item, "orderId");
            t.side = parseSide(stringField(item, "side"));
            t.price = decimalField(item, "price");
            t.qty = decimalField(item, "qty");
            t.realizedPnl = decimalField(item, "realizedPnl");
            t.marginAsset = stringField(item, "marginAsset");
            t.quoteQty = decimalField(item, "quoteQty");
            t.commission = decimalField(item, "commission");
            t.commissionAsset = stringField(item, "commissionAsset");
            t.time = intField(item, "time");
            t.positionSide = parsePositionSide(stringField(item, "positionSide"));
            t.maker = boolField(item, "maker");
            t.buyer = boolField(item, "buyer");
            out.push_back(std::move(t));
        }
        return out;
    });
}

asio::awaitable<Result<BatchOrderResult>> RestClient::batchOrders(std::vector<OrderRequest> reqs) {
    std::vector<OrderRequest> limited;
    const size_t maxBatch = std::min<size_t>(5, reqs.size());
    limited.reserve(maxBatch);
    for (size_t i = 0; i < maxBatch; ++i) {
        limited.push_back(std::move(reqs[i]));
    }

    std::ostringstream json;
    json << "[";
    std::optional<int64_t> recvWindow;
    std::vector<std::pair<std::string, std::string>> outerParams;
    for (size_t i = 0; i < limited.size(); ++i) {
        const auto& req = limited[i];
        if (!recvWindow && req.recvWindow) {
            recvWindow = req.recvWindow;
        }
        if (i > 0) json << ",";
        json << "{";
        json << "\"symbol\":\"" << jsonEscape(upper(req.symbol)) << "\",";
        json << "\"side\":\"" << sideToString(req.side) << "\",";
        json << "\"type\":\"" << typeToString(req.type) << "\",";
        json << "\"positionSide\":\"" << positionSideToString(req.positionSide) << "\",";
        json << "\"quantity\":\"" << jsonEscape(req.quantity) << "\"";
        if (req.price) appendJsonStringField(json, "price", *req.price);
        if (req.stopPrice) appendJsonStringField(json, "stopPrice", *req.stopPrice);
        if (req.activationPrice) appendJsonStringField(json, "activationPrice", *req.activationPrice);
        if (req.callbackRate) appendJsonStringField(json, "callbackRate", *req.callbackRate);
        if (req.timeInForce) appendJsonStringField(json, "timeInForce", tifToString(*req.timeInForce));
        if (req.reduceOnly) appendJsonStringField(json, "reduceOnly", boolParam(*req.reduceOnly));
        if (req.closePosition) appendJsonStringField(json, "closePosition", boolParam(*req.closePosition));
        if (req.workingType) appendJsonStringField(json, "workingType", workingTypeToString(*req.workingType));
        if (req.newClientOrderId) appendJsonStringField(json, "newClientOrderId", *req.newClientOrderId);
        if (req.newOrderRespType) appendJsonStringField(json, "newOrderRespType", *req.newOrderRespType);
        for (const auto& [k, v] : req.extraParams) {
            if (k == "recvWindow") {
                if (!recvWindow) {
                    try {
                        recvWindow = std::stoll(v);
                    } catch (const std::exception&) {
                    }
                }
                continue;
            }
            if (k == "timestamp" || k == "signature") {
                outerParams.emplace_back(k, v);
                continue;
            }
            appendJsonStringField(json, k, v);
        }
        json << "}";
    }
    json << "]";

    std::string params;
    appendParam(params, "batchOrders", json.str());
    if (recvWindow) {
        appendParam(params, "recvWindow", std::to_string(*recvWindow));
    }
    for (const auto& [k, v] : outerParams) {
        appendParam(params, k, v);
    }
    auto body = co_await signedPost("/fapi/v1/batchOrders", params);
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<BatchOrderResult>(*body, [](simdjson::ondemand::document& doc) {
        BatchOrderResult result;
        auto array = doc.get_array().value();
        for (auto itemValue : array) {
            auto itemObj = itemValue.get_object();
            if (itemObj.error()) {
                result.results.push_back(std::unexpected(BinanceError::fromParse("Invalid batch order item")));
                continue;
            }
            auto item = itemObj.value();
            const auto errCode = intField(item, "code", 0);
            if (errCode != 0) {
                result.results.push_back(std::unexpected(BinanceError::fromApiResponse(
                    static_cast<int>(errCode), stringField(item, "msg"))));
                continue;
            }
            result.results.push_back(parseOrder(item));
        }
        return result;
    });
}

asio::awaitable<Result<std::string>> RestClient::createListenKey() {
    auto body = co_await m_session->post("/fapi/v1/listenKey", {}, m_cfg.apiKey);
    if (!body) co_return std::unexpected(body.error());
    co_return parseResponse<std::string>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return stringField(object, "listenKey");
    });
}

asio::awaitable<Result<void>> RestClient::keepAliveListenKey(std::string listenKey) {
    auto body = co_await m_session->put("/fapi/v1/listenKey", query({{"listenKey", listenKey}}), m_cfg.apiKey);
    if (!body) co_return std::unexpected(body.error());
    co_return Result<void>{};
}

asio::awaitable<Result<void>> RestClient::deleteListenKey(std::string listenKey) {
    auto body = co_await m_session->del("/fapi/v1/listenKey", query({{"listenKey", listenKey}}), m_cfg.apiKey);
    if (!body) co_return std::unexpected(body.error());
    co_return Result<void>{};
}
