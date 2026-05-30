#include "ws/ws_client.h"
#include "ws/ws_parse_helpers.h"

#include <simdjson.h>

#include <algorithm>
#include <sstream>
#include <vector>

namespace {

std::string wsHost(bool testnet) {
    return testnet ? "fstream.binancefuture.com" : "fstream.binance.com";
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::vector<std::pair<double, double>> parseLevels(simdjson::dom::element levels) {
    std::vector<std::pair<double, double>> out;
    auto arr = levels.get_array();
    if (arr.error()) return out;
    for (simdjson::dom::element row : arr.value()) {
        auto level = row.get_array();
        if (level.error()) continue;
        auto it = level.value().begin();
        if (it == level.value().end()) continue;
        double price = ws_parse::toDouble(*it);
        ++it;
        if (it == level.value().end()) continue;
        out.emplace_back(price, ws_parse::toDouble(*it));
    }
    return out;
}

MarketEvent parseMarketEvent(std::string_view stream, simdjson::dom::element data) {
    const std::string streamLower = lower(std::string(stream));
    const auto eventType = ws_parse::stringField(data, "e");
    if (eventType == "aggTrade" || streamLower.find("@aggtrade") != std::string::npos) {
        AggTradeEvent e;
        e.symbol = ws_parse::stringField(data, "s");
        e.aggTradeId = ws_parse::intField(data, "a");
        e.price = ws_parse::doubleField(data, "p");
        e.qty = ws_parse::doubleField(data, "q");
        e.time = ws_parse::intField(data, "T");
        e.isBuyerMaker = ws_parse::boolField(data, "m");
        return e;
    }
    if (eventType == "kline" || streamLower.find("@kline_") != std::string::npos) {
        KlineEvent e;
        e.symbol = ws_parse::stringField(data, "s");
        simdjson::dom::element k;
        if (data["k"].get(k)) return e;
        e.interval = ws_parse::stringField(k, "i");
        e.kline.openTime = ws_parse::intField(k, "t");
        e.kline.closeTime = ws_parse::intField(k, "T");
        e.kline.open = ws_parse::doubleField(k, "o");
        e.kline.high = ws_parse::doubleField(k, "h");
        e.kline.low = ws_parse::doubleField(k, "l");
        e.kline.close = ws_parse::doubleField(k, "c");
        e.kline.volume = ws_parse::doubleField(k, "v");
        e.kline.quoteVolume = ws_parse::doubleField(k, "q");
        e.kline.quoteAssetVolume = e.kline.quoteVolume;
        e.kline.tradeCount = static_cast<int32_t>(ws_parse::intField(k, "n"));
        e.kline.numberOfTrades = e.kline.tradeCount;
        e.kline.isClosed = ws_parse::boolField(k, "x");
        return e;
    }
    if (eventType == "markPriceUpdate" || streamLower.find("@markprice") != std::string::npos) {
        MarkPriceEvent e;
        e.symbol = ws_parse::stringField(data, "s");
        e.markPrice = ws_parse::doubleField(data, "p");
        e.indexPrice = ws_parse::doubleField(data, "i");
        e.estimatedSettlePrice = ws_parse::doubleField(data, "P");
        e.fundingRate = ws_parse::doubleField(data, "r");
        e.nextFundingTime = ws_parse::intField(data, "T");
        e.time = ws_parse::intField(data, "E");
        return e;
    }
    if (eventType == "bookTicker" || streamLower.find("@bookticker") != std::string::npos) {
        BookTickerEvent e;
        e.symbol = ws_parse::stringField(data, "s");
        e.bidPrice = ws_parse::doubleField(data, "b");
        e.bidQty = ws_parse::doubleField(data, "B");
        e.askPrice = ws_parse::doubleField(data, "a");
        e.askQty = ws_parse::doubleField(data, "A");
        e.transactTime = ws_parse::intField(data, "T");
        return e;
    }
    if (eventType == "depthUpdate" || streamLower.find("@depth") != std::string::npos) {
        DepthEvent e;
        e.symbol = ws_parse::stringField(data, "s");
        e.firstUpdateId = ws_parse::intField(data, "U");
        e.finalUpdateId = ws_parse::intField(data, "u");
        e.prevFinalUpdateId = ws_parse::intField(data, "pu");
        simdjson::dom::element bids;
        simdjson::dom::element asks;
        if (!data["b"].get(bids)) e.bids = parseLevels(bids);
        if (!data["a"].get(asks)) e.asks = parseLevels(asks);
        return e;
    }
    if (eventType == "forceOrder" || streamLower.find("@forceorder") != std::string::npos) {
        LiquidationEvent e;
        simdjson::dom::element o;
        if (data["o"].get(o)) return e;
        e.symbol = ws_parse::stringField(o, "s");
        e.side = ws_parse::parseSide(ws_parse::stringField(o, "S"));
        e.type = ws_parse::parseOrderType(ws_parse::stringField(o, "o"));
        e.timeInForce = ws_parse::parseTimeInForce(ws_parse::stringField(o, "f"));
        e.status = ws_parse::stringField(o, "X");
        e.price = ws_parse::doubleField(o, "p");
        e.origQty = ws_parse::doubleField(o, "q");
        e.lastFilledQty = ws_parse::doubleField(o, "l");
        e.avgPrice = ws_parse::doubleField(o, "ap");
        e.cumFilledQty = ws_parse::doubleField(o, "z");
        e.time = ws_parse::intField(o, "T");
        return e;
    }

    CompositeIndexEvent e;
    e.symbol = ws_parse::stringField(data, "s");
    e.price = ws_parse::doubleField(data, "p");
    e.time = ws_parse::intField(data, "E");
    return e;
}

} // namespace

WsClient::WsClient(boost::asio::io_context& ioc, boost::asio::ssl::context& ssl, ContextConfig cfg)
    : m_ioc(ioc), m_ssl(ssl), m_cfg(std::move(cfg)) {}

WsClient::~WsClient() {
    m_aliveToken->store(false);
    disconnect();
}

void WsClient::subscribeAggTrade(std::string symbol, MarketEventCb cb) {
    std::lock_guard lock(m_stateMutex);
    m_subscriptions[lower(symbol) + "@aggtrade"] = std::move(cb);
}

void WsClient::subscribeKline(std::string symbol, std::string interval, MarketEventCb cb) {
    std::lock_guard lock(m_stateMutex);
    m_subscriptions[lower(symbol) + "@kline_" + lower(interval)] = std::move(cb);
}

void WsClient::subscribeMarkPrice(std::string symbol, MarketEventCb cb) {
    std::lock_guard lock(m_stateMutex);
    m_subscriptions[lower(symbol) + "@markprice"] = std::move(cb);
}

void WsClient::subscribeBookTicker(std::string symbol, MarketEventCb cb) {
    std::lock_guard lock(m_stateMutex);
    m_subscriptions[lower(symbol) + "@bookticker"] = std::move(cb);
}

void WsClient::subscribeDepth(std::string symbol, int levels, std::string updateSpeed, MarketEventCb cb) {
    std::lock_guard lock(m_stateMutex);
    m_subscriptions[lower(symbol) + "@depth" + std::to_string(levels) + "@" + lower(updateSpeed)] = std::move(cb);
}

void WsClient::subscribeLiquidation(std::string symbol, MarketEventCb cb) {
    std::lock_guard lock(m_stateMutex);
    m_subscriptions[lower(symbol) + "@forceorder"] = std::move(cb);
}

void WsClient::subscribeCompositeIndex(std::string symbol, MarketEventCb cb) {
    std::lock_guard lock(m_stateMutex);
    m_subscriptions[lower(symbol) + "@compositeindex"] = std::move(cb);
}

void WsClient::unsubscribe(std::string streamName) {
    streamName = lower(std::move(streamName));
    std::shared_ptr<WsSession> session;
    {
        std::lock_guard lock(m_stateMutex);
        m_subscriptions.erase(streamName);
        session = m_session;
    }
    if (session) {
        session->send("{\"method\":\"UNSUBSCRIBE\",\"params\":[\"" + streamName + "\"],\"id\":1}");
    }
}

void WsClient::unsubscribeAll() {
    std::vector<std::string> streams;
    std::shared_ptr<WsSession> session;
    {
        std::lock_guard lock(m_stateMutex);
        streams.reserve(m_subscriptions.size());
        for (const auto& [stream, _] : m_subscriptions) {
            streams.push_back(stream);
        }
        m_subscriptions.clear();
        session = m_session;
    }
    if (!session) {
        return;
    }
    for (const auto& stream : streams) {
        session->send("{\"method\":\"UNSUBSCRIBE\",\"params\":[\"" + stream + "\"],\"id\":1}");
    }
}

void WsClient::setOnDisconnect(std::function<void()> cb) {
    std::lock_guard lock(m_stateMutex);
    m_onDisconnect = std::move(cb);
}

void WsClient::setOnReconnect(std::function<void()> cb) {
    std::lock_guard lock(m_stateMutex);
    m_onReconnect = std::move(cb);
}

std::string WsClient::buildStreamPath() const {
    std::lock_guard lock(m_stateMutex);
    std::ostringstream out;
    out << "/stream?streams=";
    bool first = true;
    for (const auto& [stream, _] : m_subscriptions) {
        if (!first) out << '/';
        first = false;
        out << stream;
    }
    return out.str();
}

void WsClient::connect() {
    std::string path;
    std::shared_ptr<WsSession> oldSession;
    {
        std::lock_guard lock(m_stateMutex);
        if (m_subscriptions.empty()) {
            return;
        }
        oldSession = std::move(m_session);
        m_session = std::make_shared<WsSession>(m_ioc, m_ssl, wsHost(m_cfg.testnet), m_cfg.socks5Proxy);
    }
    path = buildStreamPath();

    if (oldSession) {
        oldSession->stop();
    }

    std::shared_ptr<WsSession> session;
    {
        std::lock_guard lock(m_stateMutex);
        session = m_session;
    }
    if (!session) {
        return;
    }
    auto aliveToken = m_aliveToken;
    session->start(
        path,
        [this, aliveToken](auto ec, auto raw) {
            if (!aliveToken->load()) {
                return;
            }
            onRawMessage(ec, raw);
        },
        [this, aliveToken] {
            if (!aliveToken->load()) {
                return;
            }
            std::function<void()> onDisconnect;
            {
                std::lock_guard lock(m_stateMutex);
                onDisconnect = m_onDisconnect;
            }
            if (onDisconnect) {
                onDisconnect();
            }
        },
        [this, guard] {
            std::lock_guard<std::mutex> lock(guard->mutex);
            if (!guard->alive) {
                return;
            }
            std::function<void()> onReconnect;
            {
                std::lock_guard stateLock(m_stateMutex);
                onReconnect = m_onReconnect;
            }
            if (onReconnect) {
                onReconnect();
            }
        });
}

void WsClient::disconnect() {
    std::shared_ptr<WsSession> session;
    {
        std::lock_guard lock(m_stateMutex);
        session = std::move(m_session);
    }
    if (session) {
        session->stop();
    }
}

void WsClient::onRawMessage(boost::system::error_code ec, std::string_view raw) {
    const auto subscriptions = snapshotSubscriptions();
    if (ec) {
        for (const auto& [_, cb] : subscriptions) {
            cb(ec, MarketEvent{AggTradeEvent{}});
        }
        return;
    }

    std::string stream;
    MarketEvent parsedEvent;
    boost::system::error_code callbackError{};
    bool hasEvent = false;

    {
        std::scoped_lock lock(m_parserMutex);
        simdjson::padded_string padded(raw);
        simdjson::dom::element doc;
        auto parseError = m_parser.parse(padded).get(doc);
        if (parseError) {
            callbackError = boost::asio::error::invalid_argument;
        } else {
            stream = lower(ws_parse::stringField(doc, "stream"));
            simdjson::dom::element data;
            if (doc["data"].get(data)) {
                callbackError = boost::asio::error::invalid_argument;
            } else {
                try {
                    parsedEvent = parseMarketEvent(stream, data);
                    hasEvent = true;
                } catch (const std::exception&) {
                    callbackError = boost::asio::error::invalid_argument;
                }
            }
        }
    }

    if (callbackError) {
        for (const auto& [_, cb] : subscriptions) {
            cb(callbackError, MarketEvent{AggTradeEvent{}});
        }
        return;
    }

    const auto it = subscriptions.find(stream);
    if (it == subscriptions.end()) {
        return;
    }
    if (hasEvent) {
        it->second({}, std::move(parsedEvent));
    }
}

std::map<std::string, MarketEventCb> WsClient::snapshotSubscriptions() const {
    std::lock_guard lock(m_stateMutex);
    return m_subscriptions;
}
