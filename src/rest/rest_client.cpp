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
#include <system_error>

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

int64_t intervalToMs(std::string_view interval) {
    if (interval.size() < 2) {
        return 0;
    }
    const char suffix = interval.back();
    int value = 0;
    const auto number = interval.substr(0, interval.size() - 1);
    const auto* begin = number.data();
    const auto* end = number.data() + number.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value <= 0) {
        return 0;
    }
    switch (suffix) {
        case 'm':
            return static_cast<int64_t>(value) * 60LL * 1000LL;
        case 'h':
            return static_cast<int64_t>(value) * 60LL * 60LL * 1000LL;
        case 'd':
            return static_cast<int64_t>(value) * 24LL * 60LL * 60LL * 1000LL;
        case 'w':
            return static_cast<int64_t>(value) * 7LL * 24LL * 60LL * 60LL * 1000LL;
        default:
            return 0;
    }
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
    k.isClosed = false;
    return k;
}

void markKlineClosedFlags(
    std::vector<Kline>& klines,
    std::string_view interval,
    std::optional<int64_t> endTime) {
    const int64_t stepMs = intervalToMs(interval);
    for (size_t i = 0; i < klines.size(); ++i) {
        auto& k = klines[i];
        if (stepMs > 0 && k.closeTime <= 0 && k.openTime > 0) {
            k.closeTime = k.openTime + stepMs - 1;
        }
        if (endTime) {
            k.isClosed = k.closeTime > 0 && k.closeTime <= *endTime;
            continue;
        }
        // Without a server-time reference, Binance's latest row may still be forming.
        // Treat only rows followed by a later row as definitely closed.
        k.isClosed = (i + 1) < klines.size();
    }
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

Order parseAlgoOrder(simdjson::ondemand::object& doc) {
    Order order;
    order.symbol = stringField(doc, "symbol");
    order.clientOrderId = stringField(doc, "clientAlgoId");
    order.orderId = intField(doc, "algoId");
    order.side = parseSide(stringField(doc, "side"));
    order.type = parseOrderType(stringField(doc, "orderType"));
    order.positionSide = parsePositionSide(stringField(doc, "positionSide"));
    order.timeInForce = parseTimeInForce(stringField(doc, "timeInForce"));
    order.status = stringField(doc, "algoStatus");
    order.price = decimalField(doc, "price");
    order.origQty = decimalField(doc, "quantity");
    order.reduceOnly = boolField(doc, "reduceOnly");
    order.closePosition = boolField(doc, "closePosition");
    order.stopPrice = decimalField(doc, "triggerPrice");
    order.activationPrice = decimalField(doc, "activatePrice");
    order.priceRate = decimalField(doc, "callbackRate");
    order.workingType = parseWorkingType(stringField(doc, "workingType"));
    order.time = intField(doc, "createTime");
    order.updateTime = intField(doc, "updateTime");
    return order;
}

Balance parseBalance(simdjson::ondemand::object& doc) {
    Balance b;
    b.asset = stringField(doc, "asset");
    b.walletBalanceRaw = decimalField(doc, "walletBalance");
    b.walletBalance = doubleField(doc, "walletBalance");
    b.crossWalletBalanceRaw = decimalField(doc, "crossWalletBalance");
    b.crossWalletBalance = doubleField(doc, "crossWalletBalance");
    b.unrealizedProfitRaw = decimalField(doc, "unrealizedProfit");
    b.unrealizedProfit = doubleField(doc, "unrealizedProfit");
    b.marginBalanceRaw = decimalField(doc, "marginBalance");
    b.marginBalance = doubleField(doc, "marginBalance");
    b.maintMarginRaw = decimalField(doc, "maintMargin");
    b.maintMargin = doubleField(doc, "maintMargin");
    b.initialMarginRaw = decimalField(doc, "initialMargin");
    b.initialMargin = doubleField(doc, "initialMargin");
    b.availableBalanceRaw = decimalField(doc, "availableBalance");
    b.availableBalance = doubleField(doc, "availableBalance");
    b.maxWithdrawAmountRaw = decimalField(doc, "maxWithdrawAmount");
    b.maxWithdrawAmount = doubleField(doc, "maxWithdrawAmount");
    return b;
}

Position parsePosition(simdjson::ondemand::object& doc) {
    Position p;
    p.symbol = stringField(doc, "symbol");
    p.positionSide = parsePositionSide(stringField(doc, "positionSide"));
    p.positionAmtRaw = decimalField(doc, "positionAmt");
    p.positionAmt = doubleField(doc, "positionAmt");
    p.entryPriceRaw = decimalField(doc, "entryPrice");
    p.entryPrice = doubleField(doc, "entryPrice");
    p.breakEvenPriceRaw = decimalField(doc, "breakEvenPrice");
    p.breakEvenPrice = doubleField(doc, "breakEvenPrice");
    p.markPriceRaw = decimalField(doc, "markPrice");
    p.markPrice = doubleField(doc, "markPrice");
    p.unrealizedProfitRaw = decimalField(doc, "unrealizedProfit");
    p.unrealizedProfit = doubleField(doc, "unrealizedProfit");
    p.liquidationPriceRaw = decimalField(doc, "liquidationPrice");
    p.liquidationPrice = doubleField(doc, "liquidationPrice");
    p.leverage = static_cast<int>(intField(doc, "leverage"));
    p.marginType = stringField(doc, "marginType");
    p.isolatedMarginRaw = decimalField(doc, "isolatedMargin");
    p.isolatedMargin = doubleField(doc, "isolatedMargin");
    p.initialMarginRaw = decimalField(doc, "initialMargin");
    p.initialMargin = doubleField(doc, "initialMargin");
    p.maintMarginRaw = decimalField(doc, "maintMargin");
    p.maintMargin = doubleField(doc, "maintMargin");
    p.notionalRaw = decimalField(doc, "notional");
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
      m_signer(std::move(cfg.secretKey), cfg.signingMethod),
      m_rateLimiter(sharedRateLimiter ? std::move(sharedRateLimiter) : std::make_shared<RateLimiter>()),
      m_cfg(std::move(cfg)) {
    m_cfg.clearSecretKey();
}

RestClient::RawParseResult RestClient::rawParse(std::string_view body) {
    auto storage = std::make_shared<RawParseStorage>();
    storage->buffer = simdjson::padded_string(body);
    auto docResult = storage->parser.iterate(storage->buffer);
    if (docResult.error()) {
        return compat::unexpected(BinanceError::fromParse(simdjson::error_message(docResult.error())));
    }
    auto doc = std::move(docResult).value();
    auto type = doc.type();
    if (type.error()) {
        return compat::unexpected(BinanceError::fromParse(simdjson::error_message(type.error())));
    }
    const auto bodyView = std::string_view(storage->buffer.data(), storage->buffer.length());
    return RawParsedDocument{std::move(storage), std::move(doc), bodyView};
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
        m_rateLimiter->release(cost);
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
    co_return compat::unexpected(BinanceError::fromApiResponse(-91002, "unexpected publicGet retry flow"));
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
    m_rateLimiter->release(cost);
    if (!result &&
        result.error().category == ErrorCategory::RateLimit &&
        (result.error().code == 429 || result.error().code == 418)) {
        m_rateLimiter->penalize(std::chrono::seconds(1));
    }
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
    m_rateLimiter->release(cost);
    if (!result &&
        result.error().category == ErrorCategory::RateLimit &&
        (result.error().code == 429 || result.error().code == 418)) {
        m_rateLimiter->penalize(std::chrono::seconds(1));
    }
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
    m_rateLimiter->release(cost);
    if (!result &&
        result.error().category == ErrorCategory::RateLimit &&
        (result.error().code == 429 || result.error().code == 418)) {
        m_rateLimiter->penalize(std::chrono::seconds(1));
    }
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
    m_rateLimiter->release(cost);
    if (!result &&
        result.error().category == ErrorCategory::RateLimit &&
        (result.error().code == 429 || result.error().code == 418)) {
        m_rateLimiter->penalize(std::chrono::seconds(1));
    }
    co_return result;
}

asio::awaitable<HttpSession::Result> RestClient::apiKeyPost(
    std::string_view path,
    std::string params,
    RateLimiter::Cost cost) {
    co_await m_rateLimiter->acquire(cost);
    auto result = co_await m_session->post(path, params, m_cfg.apiKey);
    m_rateLimiter->updateFromHeaders(
        m_session->lastUsedWeight(),
        m_session->lastUsedOrders(),
        m_session->lastUsedOrders10s());
    m_rateLimiter->release(cost);
    if (!result &&
        result.error().category == ErrorCategory::RateLimit &&
        (result.error().code == 429 || result.error().code == 418)) {
        m_rateLimiter->penalize(std::chrono::seconds(1));
    }
    co_return result;
}

asio::awaitable<HttpSession::Result> RestClient::apiKeyPut(
    std::string_view path,
    std::string params,
    RateLimiter::Cost cost) {
    co_await m_rateLimiter->acquire(cost);
    auto result = co_await m_session->put(path, params, m_cfg.apiKey);
    m_rateLimiter->updateFromHeaders(
        m_session->lastUsedWeight(),
        m_session->lastUsedOrders(),
        m_session->lastUsedOrders10s());
    m_rateLimiter->release(cost);
    if (!result &&
        result.error().category == ErrorCategory::RateLimit &&
        (result.error().code == 429 || result.error().code == 418)) {
        m_rateLimiter->penalize(std::chrono::seconds(1));
    }
    co_return result;
}

asio::awaitable<HttpSession::Result> RestClient::apiKeyDelete(
    std::string_view path,
    std::string params,
    RateLimiter::Cost cost) {
    co_await m_rateLimiter->acquire(cost);
    auto result = co_await m_session->del(path, params, m_cfg.apiKey);
    m_rateLimiter->updateFromHeaders(
        m_session->lastUsedWeight(),
        m_session->lastUsedOrders(),
        m_session->lastUsedOrders10s());
    m_rateLimiter->release(cost);
    if (!result &&
        result.error().category == ErrorCategory::RateLimit &&
        (result.error().code == 429 || result.error().code == 418)) {
        m_rateLimiter->penalize(std::chrono::seconds(1));
    }
    co_return result;
}

asio::awaitable<Result<bool>> RestClient::ping() {
    auto body = co_await publicGet("/fapi/v1/ping", {});
    if (!body) co_return compat::unexpected(body.error());
    co_return true;
}

asio::awaitable<Result<int64_t>> RestClient::serverTime() {
    auto body = co_await publicGet("/fapi/v1/time", {});
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<int64_t>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return intField(object, "serverTime");
    });
}

asio::awaitable<Result<std::vector<ExchangeSymbol>>> RestClient::exchangeInfo() {
    auto body = co_await publicGet("/fapi/v1/exchangeInfo", {});
    if (!body) co_return compat::unexpected(body.error());
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
            // WR-34: read each exchange-filter increment as the exact decimal
            // string the exchange sent, then derive the double from that same
            // string (simdjson ondemand fields cannot be read twice). The raw
            // string is preserved on the struct so tick-decimal counting at
            // order-formatting time is exact rather than reconstructed from a
            // lossy double.
            auto toNum = [](const std::string& s) -> double {
                if (s.empty()) {
                    return 0.0;
                }
                try {
                    return std::stod(s);
                } catch (...) {
                    return 0.0;
                }
            };
            auto filters = item.find_field_unordered("filters").get_array().value();
            for (auto filterValue : filters) {
                auto filter = filterValue.get_object().value();
                const auto type = stringField(filter, "filterType");
                if (type == "PRICE_FILTER") {
                    const std::string minPriceRaw = decimalField(filter, "minPrice");
                    const std::string maxPriceRaw = decimalField(filter, "maxPrice");
                    const std::string tickSizeRaw = decimalField(filter, "tickSize");
                    s.priceFilter = ExchangePriceFilter{
                        .minPrice = toNum(minPriceRaw),
                        .maxPrice = toNum(maxPriceRaw),
                        .tickSize = toNum(tickSizeRaw),
                        .minPriceRaw = minPriceRaw,
                        .maxPriceRaw = maxPriceRaw,
                        .tickSizeRaw = tickSizeRaw,
                    };
                    s.tickSize = s.priceFilter->tickSize;
                    s.tickSizeRaw = tickSizeRaw;
                } else if (type == "LOT_SIZE") {
                    const std::string minQtyRaw = decimalField(filter, "minQty");
                    const std::string maxQtyRaw = decimalField(filter, "maxQty");
                    const std::string stepSizeRaw = decimalField(filter, "stepSize");
                    s.lotSize = ExchangeLotSizeFilter{
                        .minQty = toNum(minQtyRaw),
                        .maxQty = toNum(maxQtyRaw),
                        .stepSize = toNum(stepSizeRaw),
                        .minQtyRaw = minQtyRaw,
                        .maxQtyRaw = maxQtyRaw,
                        .stepSizeRaw = stepSizeRaw,
                    };
                    s.stepSize = s.lotSize->stepSize;
                    s.minQty = s.lotSize->minQty;
                    s.maxQty = s.lotSize->maxQty;
                    s.stepSizeRaw = stepSizeRaw;
                    s.minQtyRaw = minQtyRaw;
                    s.maxQtyRaw = maxQtyRaw;
                } else if (type == "MARKET_LOT_SIZE") {
                    const std::string minQtyRaw = decimalField(filter, "minQty");
                    const std::string maxQtyRaw = decimalField(filter, "maxQty");
                    const std::string stepSizeRaw = decimalField(filter, "stepSize");
                    s.marketLotSize = ExchangeLotSizeFilter{
                        .minQty = toNum(minQtyRaw),
                        .maxQty = toNum(maxQtyRaw),
                        .stepSize = toNum(stepSizeRaw),
                        .minQtyRaw = minQtyRaw,
                        .maxQtyRaw = maxQtyRaw,
                        .stepSizeRaw = stepSizeRaw,
                    };
                    if (!s.lotSize.has_value()) {
                        s.stepSize = s.marketLotSize->stepSize;
                        s.minQty = s.marketLotSize->minQty;
                        s.maxQty = s.marketLotSize->maxQty;
                        s.stepSizeRaw = stepSizeRaw;
                        s.minQtyRaw = minQtyRaw;
                        s.maxQtyRaw = maxQtyRaw;
                    }
                } else if (type == "MIN_NOTIONAL") {
                    const std::string minNotionalRaw = decimalField(filter, "notional");
                    s.minNotionalFilter = ExchangeNotionalFilter{
                        .minNotional = toNum(minNotionalRaw),
                        .maxNotional = 0.0,
                        .applyMinToMarket = boolField(filter, "applyToMarket"),
                        .applyMaxToMarket = false,
                        .avgPriceMins = static_cast<int>(intField(filter, "avgPriceMins")),
                        .minNotionalRaw = minNotionalRaw,
                    };
                    s.minNotional = s.minNotionalFilter->minNotional;
                    s.minNotionalRaw = minNotionalRaw;
                } else if (type == "NOTIONAL") {
                    const std::string minNotionalRaw = decimalField(filter, "minNotional");
                    const std::string maxNotionalRaw = decimalField(filter, "maxNotional");
                    s.notionalFilter = ExchangeNotionalFilter{
                        .minNotional = toNum(minNotionalRaw),
                        .maxNotional = toNum(maxNotionalRaw),
                        .applyMinToMarket = boolField(filter, "applyMinToMarket"),
                        .applyMaxToMarket = boolField(filter, "applyMaxToMarket"),
                        .avgPriceMins = static_cast<int>(intField(filter, "avgPriceMins")),
                        .minNotionalRaw = minNotionalRaw,
                        .maxNotionalRaw = maxNotionalRaw,
                    };
                    if (s.minNotional <= 0.0) {
                        s.minNotional = s.notionalFilter->minNotional;
                        s.minNotionalRaw = minNotionalRaw;
                    }
                }
            }
            symbols.push_back(std::move(s));
        }
        return symbols;
    });
}

asio::awaitable<Result<OrderBook>> RestClient::orderBook(std::string symbol, int limit) {
    auto body = co_await publicGet(
        "/fapi/v1/depth",
        query({{"symbol", upper(symbol)}, {"limit", std::to_string(limit)}}),
        RateLimiter::Cost{.requestWeight = RateLimiter::depthWeight(limit)});
    if (!body) co_return compat::unexpected(body.error());
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
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<std::vector<Kline>>(*body, [interval, endTime](simdjson::ondemand::document& doc) {
        std::vector<Kline> out;
        auto rows = doc.get_array().value();
        for (auto rowValue : rows) {
            out.push_back(parseKlineArray(rowValue.get_array().value()));
        }
        markKlineClosedFlags(out, interval, endTime);
        return out;
    });
}

asio::awaitable<Result<MarkPrice>> RestClient::markPrice(std::string symbol) {
    auto body = co_await publicGet("/fapi/v1/premiumIndex", query({{"symbol", upper(symbol)}}));
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<MarkPrice>(*body, [](simdjson::ondemand::document& doc) {
        MarkPrice p;
        auto object = doc.get_object().value();
        p.symbol = stringField(object, "symbol");
        p.markPriceRaw = decimalField(object, "markPrice");
        p.markPrice = doubleField(object, "markPrice");
        p.indexPriceRaw = decimalField(object, "indexPrice");
        p.indexPrice = doubleField(object, "indexPrice");
        p.estimatedSettlePriceRaw = decimalField(object, "estimatedSettlePrice");
        p.estimatedSettlePrice = doubleField(object, "estimatedSettlePrice");
        p.fundingRateRaw = decimalField(object, "lastFundingRate");
        p.fundingRate = doubleField(object, "lastFundingRate");
        p.nextFundingTime = intField(object, "nextFundingTime");
        p.time = intField(object, "time");
        return p;
    });
}

asio::awaitable<Result<std::vector<MarkPrice>>> RestClient::allMarkPrices() {
    auto body = co_await publicGet("/fapi/v1/premiumIndex", {});
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<std::vector<MarkPrice>>(*body, [](simdjson::ondemand::document& doc) {
        std::vector<MarkPrice> out;
        auto array = doc.get_array().value();
        for (auto itemValue : array) {
            auto item = itemValue.get_object().value();
            MarkPrice p;
            p.symbol = stringField(item, "symbol");
            p.markPriceRaw = decimalField(item, "markPrice");
            p.markPrice = doubleField(item, "markPrice");
            p.indexPriceRaw = decimalField(item, "indexPrice");
            p.indexPrice = doubleField(item, "indexPrice");
            p.estimatedSettlePriceRaw = decimalField(item, "estimatedSettlePrice");
            p.estimatedSettlePrice = doubleField(item, "estimatedSettlePrice");
            p.fundingRateRaw = decimalField(item, "lastFundingRate");
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
    if (!body) co_return compat::unexpected(body.error());
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
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<Ticker24h>(*body, [](simdjson::ondemand::document& doc) {
        Ticker24h t;
        auto object = doc.get_object().value();
        t.symbol = stringField(object, "symbol");
        t.lastPriceRaw = decimalField(object, "lastPrice");
        t.lastPrice = doubleField(object, "lastPrice");
        t.priceChangeRaw = decimalField(object, "priceChange");
        t.priceChange = doubleField(object, "priceChange");
        t.priceChangePercentRaw = decimalField(object, "priceChangePercent");
        t.priceChangePercent = doubleField(object, "priceChangePercent");
        t.highPriceRaw = decimalField(object, "highPrice");
        t.highPrice = doubleField(object, "highPrice");
        t.lowPriceRaw = decimalField(object, "lowPrice");
        t.lowPrice = doubleField(object, "lowPrice");
        t.volumeRaw = decimalField(object, "volume");
        t.volume = doubleField(object, "volume");
        t.quoteVolumeRaw = decimalField(object, "quoteVolume");
        t.quoteVolume = doubleField(object, "quoteVolume");
        t.openTime = intField(object, "openTime");
        t.closeTime = intField(object, "closeTime");
        return t;
    });
}

asio::awaitable<Result<std::vector<Ticker24h>>> RestClient::allTicker24h() {
    auto body = co_await publicGet("/fapi/v1/ticker/24hr", {});
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<std::vector<Ticker24h>>(*body, [](simdjson::ondemand::document& doc) {
        std::vector<Ticker24h> out;
        auto array = doc.get_array().value();
        for (auto itemValue : array) {
            auto item = itemValue.get_object().value();
            Ticker24h t;
            t.symbol = stringField(item, "symbol");
            t.lastPriceRaw = decimalField(item, "lastPrice");
            t.lastPrice = doubleField(item, "lastPrice");
            t.priceChangeRaw = decimalField(item, "priceChange");
            t.priceChange = doubleField(item, "priceChange");
            t.priceChangePercentRaw = decimalField(item, "priceChangePercent");
            t.priceChangePercent = doubleField(item, "priceChangePercent");
            t.highPriceRaw = decimalField(item, "highPrice");
            t.highPrice = doubleField(item, "highPrice");
            t.lowPriceRaw = decimalField(item, "lowPrice");
            t.lowPrice = doubleField(item, "lowPrice");
            t.volumeRaw = decimalField(item, "volume");
            t.volume = doubleField(item, "volume");
            t.quoteVolumeRaw = decimalField(item, "quoteVolume");
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
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<double>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return doubleField(object, "bidPrice");
    });
}

asio::awaitable<Result<FuturesAccount>> RestClient::account() {
    auto body = co_await signedGet("/fapi/v2/account", {});
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<FuturesAccount>(*body, [](simdjson::ondemand::document& doc) {
        FuturesAccount a;
        auto object = doc.get_object().value();
        a.feeTier = doubleField(object, "feeTier");
        a.canTrade = boolField(object, "canTrade");
        a.canDeposit = boolField(object, "canDeposit");
        a.canWithdraw = boolField(object, "canWithdraw");
        a.totalWalletBalanceRaw = decimalField(object, "totalWalletBalance");
        a.totalWalletBalance = doubleField(object, "totalWalletBalance");
        a.totalUnrealizedProfitRaw = decimalField(object, "totalUnrealizedProfit");
        a.totalUnrealizedProfit = doubleField(object, "totalUnrealizedProfit");
        a.totalMarginBalanceRaw = decimalField(object, "totalMarginBalance");
        a.totalMarginBalance = doubleField(object, "totalMarginBalance");
        a.totalInitialMarginRaw = decimalField(object, "totalInitialMargin");
        a.totalInitialMargin = doubleField(object, "totalInitialMargin");
        a.totalMaintMarginRaw = decimalField(object, "totalMaintMargin");
        a.totalMaintMargin = doubleField(object, "totalMaintMargin");
        a.availableBalanceRaw = decimalField(object, "availableBalance");
        a.availableBalance = doubleField(object, "availableBalance");
        a.maxWithdrawAmountRaw = decimalField(object, "maxWithdrawAmount");
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
    if (!body) co_return compat::unexpected(body.error());
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
    if (!body) co_return compat::unexpected(body.error());
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
    if (!body) co_return compat::unexpected(body.error());
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
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<PositionModeStatus>(*body, [](simdjson::ondemand::document& doc) {
        PositionModeStatus mode;
        auto object = doc.get_object().value();
        mode.dualSidePosition = boolField(object, "dualSidePosition");
        return mode;
    });
}

asio::awaitable<Result<MultiAssetsModeStatus>> RestClient::multiAssetsMode() {
    auto body = co_await signedGet("/fapi/v1/multiAssetsMargin", {});
    if (!body) co_return compat::unexpected(body.error());
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
    if (!body) co_return compat::unexpected(body.error());
    co_return Result<void>{};
}

asio::awaitable<Result<std::vector<SymbolLeverageBrackets>>> RestClient::leverageBrackets(
    std::optional<std::string> symbol) {
    auto body = co_await signedGet("/fapi/v1/leverageBracket", symbol ? query({{"symbol", upper(*symbol)}}) : "");
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<std::vector<SymbolLeverageBrackets>>(
        *body,
        [](simdjson::ondemand::document& doc) {
            std::vector<SymbolLeverageBrackets> out;
            auto array = doc.get_array().value();
            for (auto itemValue : array) {
                auto item = itemValue.get_object().value();
                SymbolLeverageBrackets entry;
                entry.symbol = stringField(item, "symbol");
                entry.notionalCoefRaw = decimalField(item, "notionalCoef");
                entry.notionalCoef = doubleField(item, "notionalCoef");
                auto brackets = item.find_field_unordered("brackets").get_array().value();
                for (auto bracketValue : brackets) {
                    auto bracketObj = bracketValue.get_object().value();
                    LeverageBracketTier tier;
                    tier.bracket = static_cast<int>(intField(bracketObj, "bracket"));
                    tier.initialLeverage = static_cast<int>(intField(bracketObj, "initialLeverage"));
                    tier.notionalCapRaw = decimalField(bracketObj, "notionalCap");
                    tier.notionalCap = doubleField(bracketObj, "notionalCap");
                    tier.notionalFloorRaw = decimalField(bracketObj, "notionalFloor");
                    tier.notionalFloor = doubleField(bracketObj, "notionalFloor");
                    tier.maintMarginRatioRaw = decimalField(bracketObj, "maintMarginRatio");
                    tier.maintMarginRatio = doubleField(bracketObj, "maintMarginRatio");
                    tier.cumRaw = decimalField(bracketObj, "cum");
                    tier.cum = doubleField(bracketObj, "cum");
                    tier.qtyCapRaw = decimalField(bracketObj, "qtyCap");
                    tier.qtyCap = doubleField(bracketObj, "qtyCap");
                    tier.qtyFloorRaw = decimalField(bracketObj, "qtyFloor");
                    tier.qtyFloor = doubleField(bracketObj, "qtyFloor");
                    entry.brackets.push_back(tier);
                }
                out.push_back(std::move(entry));
            }
            return out;
        });
}

asio::awaitable<Result<LeverageResult>> RestClient::setLeverage(std::string symbol, int leverage) {
    auto body = co_await signedPost("/fapi/v1/leverage", query({{"symbol", upper(symbol)}, {"leverage", std::to_string(leverage)}}));
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<LeverageResult>(*body, [](simdjson::ondemand::document& doc) {
        LeverageResult r;
        auto object = doc.get_object().value();
        r.symbol = stringField(object, "symbol");
        r.leverage = static_cast<int>(intField(object, "leverage"));
        r.maxNotionalValueRaw = decimalField(object, "maxNotionalValue");
        r.maxNotionalValue = doubleField(object, "maxNotionalValue");
        return r;
    });
}

asio::awaitable<Result<void>> RestClient::setMarginType(std::string symbol, std::string marginType) {
    auto body = co_await signedPost("/fapi/v1/marginType", query({{"symbol", upper(symbol)}, {"marginType", upper(marginType)}}));
    if (!body) co_return compat::unexpected(body.error());
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
    auto body = co_await signedPost(
        "/fapi/v1/order",
        params,
        RateLimiter::Cost{.requestWeight = 1, .orders1m = 1, .orders10s = 1});
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseOrder(object);
    });
}

asio::awaitable<Result<Order>> RestClient::newAlgoOrder(OrderRequest req) {
    std::string params;
    appendParam(params, "algoType", "CONDITIONAL");
    appendParam(params, "symbol", upper(req.symbol));
    appendParam(params, "side", sideToString(req.side));
    appendParam(params, "type", typeToString(req.type));
    appendParam(params, "positionSide", positionSideToString(req.positionSide));
    appendParam(params, "quantity", req.quantity);
    appendParam(params, "price", req.price.value_or(""));
    appendParam(params, "triggerPrice", req.stopPrice.value_or(""));
    appendParam(params, "activatePrice", req.activationPrice.value_or(""));
    appendParam(params, "callbackRate", req.callbackRate.value_or(""));
    appendParam(params, "timeInForce", req.timeInForce ? tifToString(*req.timeInForce) : "");
    appendParam(params, "reduceOnly", req.reduceOnly ? boolParam(*req.reduceOnly) : "");
    appendParam(params, "closePosition", req.closePosition ? boolParam(*req.closePosition) : "");
    appendParam(params, "workingType", req.workingType ? workingTypeToString(*req.workingType) : "");
    appendParam(params, "clientAlgoId", req.newClientOrderId.value_or(""));
    appendParam(params, "newOrderRespType", req.newOrderRespType.value_or(""));
    appendParam(params, "recvWindow", req.recvWindow ? std::to_string(*req.recvWindow) : "");
    for (const auto& [k, v] : req.extraParams) {
        appendParam(params, k, v);
    }
    auto body = co_await signedPost(
        "/fapi/v1/algoOrder",
        params,
        RateLimiter::Cost{.requestWeight = 1, .orders1m = 1, .orders10s = 1});
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseAlgoOrder(object);
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
    auto body = co_await signedPut(
        "/fapi/v1/order",
        params,
        RateLimiter::Cost{.requestWeight = 1, .orders1m = 1, .orders10s = 1});
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseOrder(object);
    });
}

asio::awaitable<Result<Order>> RestClient::cancelOrder(std::string symbol, int64_t orderId) {
    auto body = co_await signedDelete(
        "/fapi/v1/order",
        query({{"symbol", upper(symbol)}, {"orderId", std::to_string(orderId)}}),
        RateLimiter::Cost{.requestWeight = 1, .orders1m = 1, .orders10s = 1});
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseOrder(object);
    });
}

asio::awaitable<Result<Order>> RestClient::cancelAlgoOrder(std::string symbol, int64_t algoId) {
    (void)symbol;
    auto body = co_await signedDelete(
        "/fapi/v1/algoOrder",
        query({{"algoId", std::to_string(algoId)}}),
        RateLimiter::Cost{.requestWeight = 1, .orders1m = 1, .orders10s = 1});
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseAlgoOrder(object);
    });
}

asio::awaitable<Result<Order>> RestClient::cancelOrderByClientOrderId(std::string symbol, std::string clientOrderId) {
    auto body = co_await signedDelete(
        "/fapi/v1/order",
        query({{"symbol", upper(symbol)}, {"origClientOrderId", clientOrderId}}),
        RateLimiter::Cost{.requestWeight = 1, .orders1m = 1, .orders10s = 1});
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseOrder(object);
    });
}

asio::awaitable<Result<Order>> RestClient::cancelAlgoOrderByClientAlgoId(
    std::string symbol,
    std::string clientAlgoId) {
    (void)symbol;
    auto body = co_await signedDelete(
        "/fapi/v1/algoOrder",
        query({{"clientAlgoId", clientAlgoId}}),
        RateLimiter::Cost{.requestWeight = 1, .orders1m = 1, .orders10s = 1});
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseAlgoOrder(object);
    });
}

asio::awaitable<Result<void>> RestClient::cancelAllOrders(std::string symbol) {
    auto body = co_await signedDelete(
        "/fapi/v1/allOpenOrders",
        query({{"symbol", upper(symbol)}}),
        RateLimiter::Cost{.requestWeight = 1, .orders1m = 1, .orders10s = 1});
    if (!body) co_return compat::unexpected(body.error());
    co_return Result<void>{};
}

asio::awaitable<Result<Order>> RestClient::queryOrder(std::string symbol, int64_t orderId) {
    auto body = co_await signedGet("/fapi/v1/order", query({{"symbol", upper(symbol)}, {"orderId", std::to_string(orderId)}}));
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseOrder(object);
    });
}

asio::awaitable<Result<Order>> RestClient::queryAlgoOrder(std::string symbol, int64_t algoId) {
    (void)symbol;
    auto body = co_await signedGet(
        "/fapi/v1/algoOrder",
        query({{"algoId", std::to_string(algoId)}}));
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseAlgoOrder(object);
    });
}

asio::awaitable<Result<Order>> RestClient::queryOrderByClientOrderId(std::string symbol, std::string clientOrderId) {
    auto body = co_await signedGet(
        "/fapi/v1/order",
        query({{"symbol", upper(symbol)}, {"origClientOrderId", clientOrderId}}));
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseOrder(object);
    });
}

asio::awaitable<Result<Order>> RestClient::queryAlgoOrderByClientAlgoId(
    std::string symbol,
    std::string clientAlgoId) {
    (void)symbol;
    auto body = co_await signedGet(
        "/fapi/v1/algoOrder",
        query({{"clientAlgoId", clientAlgoId}}));
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<Order>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return parseAlgoOrder(object);
    });
}

asio::awaitable<Result<std::vector<Order>>> RestClient::openOrders(std::optional<std::string> symbol) {
    auto body = co_await signedGet(
        "/fapi/v1/openOrders",
        symbol ? query({{"symbol", upper(*symbol)}}) : "",
        RateLimiter::Cost{.requestWeight = symbol ? 1 : 40});
    if (!body) co_return compat::unexpected(body.error());
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
    if (!body) co_return compat::unexpected(body.error());
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
    if (!body) co_return compat::unexpected(body.error());
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
    if (reqs.size() > 5) {
        co_return compat::unexpected(BinanceError::fromApiResponse(
            -91003,
            "batchOrders supports at most 5 orders per request"));
    }
    std::vector<OrderRequest> limited = std::move(reqs);
    const size_t maxBatch = limited.size();

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
        json << "\"symbol\":\"" << jsonEscape(upper(req.symbol)) << "\"";
        appendJsonStringField(json, "side", sideToString(req.side));
        appendJsonStringField(json, "type", typeToString(req.type));
        appendJsonStringField(json, "positionSide", positionSideToString(req.positionSide));
        const bool allowQuantity = !(req.closePosition && *req.closePosition);
        if (allowQuantity) {
            appendJsonStringField(json, "quantity", req.quantity);
        }
        if (req.price) appendJsonStringField(json, "price", *req.price);
        if (req.stopPrice) appendJsonStringField(json, "stopPrice", *req.stopPrice);
        if (req.activationPrice) appendJsonStringField(json, "activationPrice", *req.activationPrice);
        if (req.callbackRate) appendJsonStringField(json, "callbackRate", *req.callbackRate);
        if (req.timeInForce) {
            appendJsonStringField(json, "timeInForce", tifToString(*req.timeInForce));
        } else if (req.type == OrderType::Limit) {
            appendJsonStringField(json, "timeInForce", "GTC");
        }
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
    auto body = co_await signedPost(
        "/fapi/v1/batchOrders",
        params,
        RateLimiter::Cost{
            .requestWeight = 5,
            .orders1m = static_cast<int>(maxBatch),
            .orders10s = static_cast<int>(maxBatch)});
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<BatchOrderResult>(*body, [](simdjson::ondemand::document& doc) {
        BatchOrderResult result;
        auto array = doc.get_array().value();
        for (auto itemValue : array) {
            auto itemObj = itemValue.get_object();
            if (itemObj.error()) {
                result.results.push_back(compat::unexpected(BinanceError::fromParse("Invalid batch order item")));
                continue;
            }
            auto item = itemObj.value();
            const auto errCode = intField(item, "code", 0);
            if (errCode != 0) {
                result.results.push_back(compat::unexpected(BinanceError::fromApiResponse(
                    static_cast<int>(errCode), stringField(item, "msg"))));
                continue;
            }
            result.results.push_back(parseOrder(item));
        }
        return result;
    });
}

asio::awaitable<Result<std::string>> RestClient::createListenKey() {
    auto body = co_await apiKeyPost("/fapi/v1/listenKey", {});
    if (!body) co_return compat::unexpected(body.error());
    co_return parseResponse<std::string>(*body, [](simdjson::ondemand::document& doc) {
        auto object = doc.get_object().value();
        return stringField(object, "listenKey");
    });
}

asio::awaitable<Result<void>> RestClient::keepAliveListenKey(std::string listenKey) {
    auto body = co_await apiKeyPut("/fapi/v1/listenKey", query({{"listenKey", listenKey}}));
    if (!body) co_return compat::unexpected(body.error());
    co_return Result<void>{};
}

asio::awaitable<Result<void>> RestClient::deleteListenKey(std::string listenKey) {
    auto body = co_await apiKeyDelete("/fapi/v1/listenKey", query({{"listenKey", listenKey}}));
    if (!body) co_return compat::unexpected(body.error());
    co_return Result<void>{};
}
