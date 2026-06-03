#include "ws/user_data_stream.h"
#include "ws/ws_parse_helpers.h"

#include "logger.h"

#include <simdjson.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <exception>

namespace {

void logCoroutineException(std::string_view name, std::exception_ptr ep) {
    if (!ep) {
        return;
    }
    try {
        std::rethrow_exception(ep);
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::Error, std::string(name) + " exception: " + e.what());
    } catch (...) {
        Logger::instance().log(LogLevel::Error, std::string(name) + " unknown exception");
    }
}

} // namespace

namespace {

std::string wsHost(bool testnet) {
    return testnet ? "fstream.binancefuture.com" : "fstream.binance.com";
}

Balance parseBalance(simdjson::dom::element doc) {
    Balance b;
    b.asset = ws_parse::stringField(doc, "a");
    b.walletBalanceRaw = ws_parse::stringField(doc, "wb", "0");
    b.walletBalance = ws_parse::doubleField(doc, "wb");
    b.crossWalletBalanceRaw = ws_parse::stringField(doc, "cw", "0");
    b.crossWalletBalance = ws_parse::doubleField(doc, "cw");
    b.unrealizedProfitRaw = ws_parse::stringField(doc, "up", "0");
    b.unrealizedProfit = ws_parse::doubleField(doc, "up");
    return b;
}

Position parsePosition(simdjson::dom::element doc) {
    Position p;
    p.symbol = ws_parse::stringField(doc, "s");
    p.positionSide = ws_parse::parsePositionSide(ws_parse::stringField(doc, "ps"));
    p.positionAmtRaw = ws_parse::stringField(doc, "pa", "0");
    p.positionAmt = ws_parse::doubleField(doc, "pa");
    p.entryPriceRaw = ws_parse::stringField(doc, "ep", "0");
    p.entryPrice = ws_parse::doubleField(doc, "ep");
    p.unrealizedProfitRaw = ws_parse::stringField(doc, "up", "0");
    p.unrealizedProfit = ws_parse::doubleField(doc, "up");
    p.marginType = ws_parse::stringField(doc, "mt");
    p.isolatedMarginRaw = ws_parse::stringField(doc, "iw", "0");
    p.isolatedMargin = ws_parse::doubleField(doc, "iw");
    return p;
}

UserDataEvent parseUserEvent(simdjson::dom::element doc) {
    const auto eventType = ws_parse::stringField(doc, "e");
    if (eventType == "ORDER_TRADE_UPDATE") {
        OrderUpdateEvent e;
        simdjson::dom::element o;
        if (doc["o"].get(o)) return e;
        e.symbol = ws_parse::stringField(o, "s");
        e.clientOrderId = ws_parse::stringField(o, "c");
        e.originalClientOrderId = ws_parse::stringField(o, "C");
        e.orderId = ws_parse::intField(o, "i");
        e.side = ws_parse::parseSide(ws_parse::stringField(o, "S"));
        e.type = ws_parse::parseOrderType(ws_parse::stringField(o, "o"));
        e.positionSide = ws_parse::parsePositionSide(ws_parse::stringField(o, "ps"));
        e.timeInForce = ws_parse::parseTimeInForce(ws_parse::stringField(o, "f"));
        e.executionType = ws_parse::stringField(o, "x");
        e.orderStatus = ws_parse::stringField(o, "X");
        e.originalQty = ws_parse::stringField(o, "q", "0");
        e.originalPrice = ws_parse::stringField(o, "p", "0");
        e.avgPrice = ws_parse::stringField(o, "ap", "0");
        e.lastFilledQty = ws_parse::stringField(o, "l", "0");
        e.lastFilledPrice = ws_parse::stringField(o, "L", "0");
        e.accumulatedFilledQty = ws_parse::stringField(o, "z", "0");
        e.realizedPnl = ws_parse::stringField(o, "rp", "0");
        e.commission = ws_parse::stringField(o, "n", "0");
        e.commissionAsset = ws_parse::stringField(o, "N");
        e.isReduceOnly = ws_parse::boolField(o, "R");
        e.closePosition = ws_parse::boolField(o, "cp");
        e.stopPrice = ws_parse::stringField(o, "sp", "0");
        e.activationPrice = ws_parse::stringField(o, "AP", "0");
        e.priceRate = ws_parse::stringField(o, "cr", "0");
        e.workingType = ws_parse::parseWorkingType(ws_parse::stringField(o, "wt"));
        e.orderTime = ws_parse::intField(o, "T");
        e.tradeTime = ws_parse::intField(o, "t");
        return e;
    }
    if (eventType == "ACCOUNT_UPDATE") {
        AccountUpdateEvent e;
        e.time = ws_parse::intField(doc, "E");
        simdjson::dom::element a;
        if (doc["a"].get(a)) return e;
        e.eventReason = ws_parse::stringField(a, "m");
        simdjson::dom::array balances;
        if (!a["B"].get(balances)) {
            for (simdjson::dom::element b : balances) e.balances.push_back(parseBalance(b));
        }
        simdjson::dom::array positions;
        if (!a["P"].get(positions)) {
            for (simdjson::dom::element p : positions) e.positions.push_back(parsePosition(p));
        }
        return e;
    }

    MarginCallEvent e;
    e.time = ws_parse::intField(doc, "E");
    simdjson::dom::array positions;
    if (!doc["p"].get(positions)) {
        for (simdjson::dom::element p : positions) e.positions.push_back(parsePosition(p));
    }
    return e;
}

} // namespace

UserDataStream::UserDataStream(boost::asio::io_context& ioc, boost::asio::ssl::context& ssl, ContextConfig cfg)
    : m_ioc(ioc),
      m_ssl(ssl),
      m_rest(std::make_shared<RestClient>(ioc, ssl, cfg)),
      m_cfg(std::move(cfg)),
      m_keepaliveTimer(ioc) {
    m_cfg.clearSecretKey();
}

UserDataStream::~UserDataStream() {
    {
        std::lock_guard<std::mutex> lock(m_callbackGuard->mutex);
        m_callbackGuard->alive = false;
    }
    stop();
}

void UserDataStream::start(UserDataCb cb) {
    {
        std::lock_guard lock(m_stateMutex);
        m_callback = std::move(cb);
        m_stopped.store(false);
    }
    boost::asio::co_spawn(
        m_ioc,
        [this] { return startAsync(); },
        [](std::exception_ptr ep) { logCoroutineException("UserDataStream startAsync", ep); });
}

boost::asio::awaitable<void> UserDataStream::startAsync() {
    auto listenKey = co_await m_rest->createListenKey();
    if (!listenKey) {
        UserDataCb callback;
        {
            std::lock_guard lock(m_stateMutex);
            callback = m_callback;
        }
        if (callback) callback(boost::asio::error::operation_aborted, OrderUpdateEvent{});
        co_return;
    }
    if (m_stopped.load()) {
        (void)co_await m_rest->deleteListenKey(*listenKey);
        co_return;
    }
    {
        std::lock_guard lock(m_stateMutex);
        m_listenKey = *listenKey;
    }
    startSessionWithCurrentListenKey();
    boost::asio::co_spawn(
        m_ioc,
        [this] { return keepaliveLoop(); },
        [](std::exception_ptr ep) { logCoroutineException("UserDataStream keepaliveLoop", ep); });
}

void UserDataStream::stop() {
    std::shared_ptr<WsSession> session;
    std::string listenKey;
    {
        std::lock_guard lock(m_stateMutex);
        m_stopped.store(true);
        session = std::move(m_session);
        listenKey = std::exchange(m_listenKey, {});
    }
    if (session) {
        session->stop();
    }
    m_keepaliveTimer.cancel();
    if (!listenKey.empty()) {
        auto rest = m_rest;
        boost::asio::co_spawn(
            m_ioc,
            [rest, listenKey]() mutable { return rest->deleteListenKey(listenKey); },
            [](std::exception_ptr ep, auto) { logCoroutineException("UserDataStream deleteListenKey", ep); });
    }
}

void UserDataStream::setOnDisconnect(std::function<void()> cb) {
    std::lock_guard lock(m_stateMutex);
    m_onDisconnect = std::move(cb);
}

void UserDataStream::setOnReconnect(std::function<void()> cb) {
    std::lock_guard lock(m_stateMutex);
    m_onReconnect = std::move(cb);
}

void UserDataStream::startSessionWithCurrentListenKey() {
    std::string listenKey;
    std::function<void()> onDisconnect;
    std::function<void()> onReconnect;
    std::shared_ptr<WsSession> session;
    {
        std::lock_guard lock(m_stateMutex);
        if (m_stopped.load() || m_listenKey.empty()) {
            return;
        }
        listenKey = m_listenKey;
        onDisconnect = m_onDisconnect;
        onReconnect = m_onReconnect;
        session = std::make_shared<WsSession>(m_ioc, m_ssl, wsHost(m_cfg.testnet), m_cfg.socks5Proxy);
        m_session = session;
    }

    session->start(
        "/private/ws?listenKey=" + listenKey,
        [this, guard = m_callbackGuard](auto ec, auto raw) {
            std::lock_guard<std::mutex> lock(guard->mutex);
            if (!guard->alive) {
                return;
            }
            onRawMessage(ec, raw);
        },
        std::move(onDisconnect),
        [this, onReconnect = std::move(onReconnect)] {
            if (m_stopped.load()) {
                return;
            }
            if (onReconnect) {
                onReconnect();
            }
        });
}

boost::asio::awaitable<void> UserDataStream::keepaliveLoop() {
    while (!m_stopped.load()) {
        std::string listenKey;
        {
            std::lock_guard lock(m_stateMutex);
            listenKey = m_listenKey;
        }
        if (listenKey.empty()) {
            co_return;
        }
        m_keepaliveTimer.expires_after(std::chrono::minutes(30));
        boost::system::error_code ec;
        co_await m_keepaliveTimer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) co_return;
        (void)co_await m_rest->keepAliveListenKey(listenKey);
    }
}

void UserDataStream::onRawMessage(boost::system::error_code ec, std::string_view raw) {
    UserDataCb callback;
    {
        std::lock_guard lock(m_stateMutex);
        callback = m_callback;
    }
    if (!callback) {
        return;
    }
    if (ec) {
        callback(ec, OrderUpdateEvent{});
        return;
    }

    UserDataEvent parsedEvent;
    boost::system::error_code parseError;
    {
        std::scoped_lock lock(m_parserMutex);
        simdjson::padded_string padded(raw);
        simdjson::dom::element doc;
        if (m_parser.parse(padded).get(doc)) {
            parseError = boost::asio::error::invalid_argument;
        } else {
            try {
                parsedEvent = parseUserEvent(doc);
            } catch (const std::exception&) {
                parseError = boost::asio::error::invalid_argument;
            }
        }
    }
    if (parseError) {
        callback(parseError, OrderUpdateEvent{});
        return;
    }
    callback({}, std::move(parsedEvent));
}
