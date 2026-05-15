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
    b.walletBalance = ws_parse::doubleField(doc, "wb");
    b.crossWalletBalance = ws_parse::doubleField(doc, "cw");
    b.unrealizedProfit = ws_parse::doubleField(doc, "up");
    return b;
}

Position parsePosition(simdjson::dom::element doc) {
    Position p;
    p.symbol = ws_parse::stringField(doc, "s");
    p.positionSide = ws_parse::parsePositionSide(ws_parse::stringField(doc, "ps"));
    p.positionAmt = ws_parse::doubleField(doc, "pa");
    p.entryPrice = ws_parse::doubleField(doc, "ep");
    p.unrealizedProfit = ws_parse::doubleField(doc, "up");
    p.marginType = ws_parse::stringField(doc, "mt");
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
        e.originalQty = ws_parse::doubleField(o, "q");
        e.originalPrice = ws_parse::doubleField(o, "p");
        e.avgPrice = ws_parse::doubleField(o, "ap");
        e.lastFilledQty = ws_parse::doubleField(o, "l");
        e.lastFilledPrice = ws_parse::doubleField(o, "L");
        e.accumulatedFilledQty = ws_parse::doubleField(o, "z");
        e.realizedPnl = ws_parse::doubleField(o, "rp");
        e.commission = ws_parse::doubleField(o, "n");
        e.commissionAsset = ws_parse::stringField(o, "N");
        e.isReduceOnly = ws_parse::boolField(o, "R");
        e.closePosition = ws_parse::boolField(o, "cp");
        e.stopPrice = ws_parse::doubleField(o, "sp");
        e.activationPrice = ws_parse::doubleField(o, "AP");
        e.priceRate = ws_parse::doubleField(o, "cr");
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
      m_rest(ioc, ssl, cfg),
      m_cfg(std::move(cfg)),
      m_keepaliveTimer(ioc) {}

void UserDataStream::start(UserDataCb cb) {
    m_callback = std::move(cb);
    boost::asio::co_spawn(
        m_ioc,
        [this] { return startAsync(); },
        [](std::exception_ptr ep) { logCoroutineException("UserDataStream startAsync", ep); });
}

boost::asio::awaitable<void> UserDataStream::startAsync() {
    auto listenKey = co_await m_rest.createListenKey();
    if (!listenKey) {
        if (m_callback) m_callback(boost::asio::error::operation_aborted, OrderUpdateEvent{});
        co_return;
    }
    m_listenKey = *listenKey;
    startSessionWithCurrentListenKey();
    boost::asio::co_spawn(
        m_ioc,
        [this] { return keepaliveLoop(); },
        [](std::exception_ptr ep) { logCoroutineException("UserDataStream keepaliveLoop", ep); });
}

void UserDataStream::stop() {
    if (m_session) {
        m_session->stop();
    }
    m_keepaliveTimer.cancel();
    if (!m_listenKey.empty()) {
        boost::asio::co_spawn(
            m_ioc,
            [this] { return m_rest.deleteListenKey(m_listenKey); },
            [](std::exception_ptr ep, auto) { logCoroutineException("UserDataStream deleteListenKey", ep); });
    }
}

void UserDataStream::setOnDisconnect(std::function<void()> cb) {
    m_onDisconnect = std::move(cb);
}

void UserDataStream::setOnReconnect(std::function<void()> cb) {
    m_onReconnect = std::move(cb);
}

void UserDataStream::startSessionWithCurrentListenKey() {
    m_session = std::make_shared<WsSession>(m_ioc, m_ssl, wsHost(m_cfg.testnet), m_cfg.socks5Proxy);
    m_session->start(
        "/ws/" + m_listenKey,
        [this](auto ec, auto raw) { onRawMessage(ec, raw); },
        m_onDisconnect,
        [this] {
            boost::asio::co_spawn(
                m_ioc,
                [this] { return refreshListenKeyAfterReconnect(); },
                [](std::exception_ptr ep) {
                    logCoroutineException("UserDataStream refreshListenKeyAfterReconnect", ep);
                });
            if (m_onReconnect) {
                m_onReconnect();
            }
        });
}

boost::asio::awaitable<void> UserDataStream::refreshListenKeyAfterReconnect() {
    bool expected = false;
    if (!m_refreshingListenKey.compare_exchange_strong(expected, true)) {
        co_return;
    }

    auto releaseGuard = [this]() { m_refreshingListenKey.store(false); };
    auto refreshed = co_await m_rest.createListenKey();
    if (!refreshed) {
        releaseGuard();
        co_return;
    }

    const std::string oldListenKey = m_listenKey;
    m_listenKey = *refreshed;
    if (m_session) {
        m_session->stop();
    }
    startSessionWithCurrentListenKey();
    if (!oldListenKey.empty() && oldListenKey != m_listenKey) {
        (void)co_await m_rest.deleteListenKey(oldListenKey);
    }
    releaseGuard();
}

boost::asio::awaitable<void> UserDataStream::keepaliveLoop() {
    while (!m_listenKey.empty()) {
        m_keepaliveTimer.expires_after(std::chrono::minutes(30));
        boost::system::error_code ec;
        co_await m_keepaliveTimer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) co_return;
        (void)co_await m_rest.keepAliveListenKey(m_listenKey);
    }
}

void UserDataStream::onRawMessage(boost::system::error_code ec, std::string_view raw) {
    if (!m_callback) {
        return;
    }
    if (ec) {
        m_callback(ec, OrderUpdateEvent{});
        return;
    }

    simdjson::dom::parser parser;
    simdjson::padded_string padded(raw);
    simdjson::dom::element doc;
    if (parser.parse(padded).get(doc)) {
        m_callback(boost::asio::error::invalid_argument, OrderUpdateEvent{});
        return;
    }
    m_callback({}, parseUserEvent(doc));
}
