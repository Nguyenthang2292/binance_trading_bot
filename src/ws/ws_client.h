#pragma once

#include "context.h"
#include "transport/ws_session.h"
#include "types/events.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <simdjson.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

using MarketEventCb = std::function<void(boost::system::error_code, MarketEvent)>;

class WsClient {
public:
    WsClient(boost::asio::io_context& ioc, boost::asio::ssl::context& ssl, ContextConfig cfg);

    void subscribeAggTrade(std::string symbol, MarketEventCb cb);
    void subscribeKline(std::string symbol, std::string interval, MarketEventCb cb);
    void subscribeMarkPrice(std::string symbol, MarketEventCb cb);
    void subscribeBookTicker(std::string symbol, MarketEventCb cb);
    void subscribeDepth(std::string symbol, int levels, std::string updateSpeed, MarketEventCb cb);
    void subscribeLiquidation(std::string symbol, MarketEventCb cb);
    void subscribeCompositeIndex(std::string symbol, MarketEventCb cb);

    void unsubscribe(std::string streamName);
    void unsubscribeAll();

    void setOnDisconnect(std::function<void()> cb);
    void setOnReconnect(std::function<void()> cb);

    void connect();
    void disconnect();

private:
    std::string buildStreamPath() const;
    void onRawMessage(boost::system::error_code ec, std::string_view raw);

    boost::asio::io_context& m_ioc;
    boost::asio::ssl::context& m_ssl;
    ContextConfig m_cfg;
    std::shared_ptr<WsSession> m_session;
    std::map<std::string, MarketEventCb> m_subscriptions;
    std::function<void()> m_onDisconnect;
    std::function<void()> m_onReconnect;
    simdjson::dom::parser m_parser;
    std::mutex m_parserMutex;
};
