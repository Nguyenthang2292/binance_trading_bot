#include "ws/ws_client.h"
#include "ws/ws_parse_helpers.h"

#include <simdjson.h>

#include <algorithm>
#include <sstream>

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
    const auto eventType = ws_parse::stringField(data, "e");
    if (eventType == "aggTrade" || stream.find("@aggTrade") != std::string_view::npos) {
        AggTradeEvent e;
        e.symbol = ws_parse::stringField(data, "s");
        e.aggTradeId = ws_parse::intField(data, "a");
        e.price = ws_parse::doubleField(data, "p");
        e.qty = ws_parse::doubleField(data, "q");
        e.time = ws_parse::intField(data, "T");
        e.isBuyerMaker = ws_parse::boolField(data, "m");
        return e;
    }
    if (eventType == "kline" || stream.find("@kline_") != std::string_view::npos) {
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
    if (eventType == "markPriceUpdate" || stream.find("@markPrice") != std::string_view::npos) {
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
    if (eventType == "bookTicker" || stream.find("@bookTicker") != std::string_view::npos) {
        BookTickerEvent e;
        e.symbol = ws_parse::stringField(data, "s");
        e.bidPrice = ws_parse::doubleField(data, "b");
        e.bidQty = ws_parse::doubleField(data, "B");
        e.askPrice = ws_parse::doubleField(data, "a");
        e.askQty = ws_parse::doubleField(data, "A");
        e.transactTime = ws_parse::intField(data, "T");
        return e;
    }
    if (eventType == "depthUpdate" || stream.find("@depth") != std::string_view::npos) {
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
    if (eventType == "forceOrder" || stream.find("@forceOrder") != std::string_view::npos) {
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

void WsClient::subscribeAggTrade(std::string symbol, MarketEventCb cb) {
    m_subscriptions[lower(symbol) + "@aggTrade"] = std::move(cb);
}

void WsClient::subscribeKline(std::string symbol, std::string interval, MarketEventCb cb) {
    m_subscriptions[lower(symbol) + "@kline_" + interval] = std::move(cb);
}

void WsClient::subscribeMarkPrice(std::string symbol, MarketEventCb cb) {
    m_subscriptions[lower(symbol) + "@markPrice"] = std::move(cb);
}

void WsClient::subscribeBookTicker(std::string symbol, MarketEventCb cb) {
    m_subscriptions[lower(symbol) + "@bookTicker"] = std::move(cb);
}

void WsClient::subscribeDepth(std::string symbol, int levels, std::string updateSpeed, MarketEventCb cb) {
    m_subscriptions[lower(symbol) + "@depth" + std::to_string(levels) + "@" + updateSpeed] = std::move(cb);
}

void WsClient::subscribeLiquidation(std::string symbol, MarketEventCb cb) {
    m_subscriptions[lower(symbol) + "@forceOrder"] = std::move(cb);
}

void WsClient::subscribeCompositeIndex(std::string symbol, MarketEventCb cb) {
    m_subscriptions[lower(symbol) + "@compositeIndex"] = std::move(cb);
}

void WsClient::unsubscribe(std::string streamName) {
    streamName = lower(std::move(streamName));
    m_subscriptions.erase(streamName);
    if (m_session) {
        m_session->send("{\"method\":\"UNSUBSCRIBE\",\"params\":[\"" + streamName + "\"],\"id\":1}");
    }
}

void WsClient::unsubscribeAll() {
    for (const auto& [stream, _] : m_subscriptions) {
        if (m_session) {
            m_session->send("{\"method\":\"UNSUBSCRIBE\",\"params\":[\"" + stream + "\"],\"id\":1}");
        }
    }
    m_subscriptions.clear();
}

void WsClient::setOnDisconnect(std::function<void()> cb) {
    m_onDisconnect = std::move(cb);
}

void WsClient::setOnReconnect(std::function<void()> cb) {
    m_onReconnect = std::move(cb);
}

std::string WsClient::buildStreamPath() const {
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
    if (m_subscriptions.empty()) {
        return;
    }
    if (m_session) {
        m_session->stop();
    }
    m_session = std::make_shared<WsSession>(m_ioc, m_ssl, wsHost(m_cfg.testnet), m_cfg.socks5Proxy);
    m_session->start(buildStreamPath(),
                     [this](auto ec, auto raw) { onRawMessage(ec, raw); },
                     m_onDisconnect,
                     [this] {
                         connect();
                         if (m_onReconnect) {
                             m_onReconnect();
                         }
                     });
}

void WsClient::disconnect() {
    if (m_session) {
        m_session->stop();
    }
}

void WsClient::onRawMessage(boost::system::error_code ec, std::string_view raw) {
    if (ec) {
        for (const auto& [_, cb] : m_subscriptions) {
            cb(ec, MarketEvent{AggTradeEvent{}});
        }
        return;
    }

    std::scoped_lock lock(m_parserMutex);
    simdjson::padded_string padded(raw);
    simdjson::dom::element doc;
    auto parseError = m_parser.parse(padded).get(doc);
    if (parseError) {
        for (const auto& [_, cb] : m_subscriptions) {
            cb(boost::asio::error::invalid_argument, MarketEvent{AggTradeEvent{}});
        }
        return;
    }

    const auto stream = ws_parse::stringField(doc, "stream");
    const auto it = m_subscriptions.find(stream);
    if (it == m_subscriptions.end()) {
        return;
    }
    simdjson::dom::element data;
    if (doc["data"].get(data)) {
        it->second(boost::asio::error::invalid_argument, MarketEvent{AggTradeEvent{}});
        return;
    }
    it->second({}, parseMarketEvent(stream, data));
}
