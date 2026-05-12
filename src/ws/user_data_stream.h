#pragma once

#include "context.h"
#include "rest/rest_client.h"
#include "transport/ws_session.h"
#include "types/events.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

using UserDataCb = std::function<void(boost::system::error_code, UserDataEvent)>;

class UserDataStream {
public:
    UserDataStream(boost::asio::io_context& ioc, boost::asio::ssl::context& ssl, ContextConfig cfg);

    void start(UserDataCb cb);
    void stop();

    void setOnDisconnect(std::function<void()> cb);
    void setOnReconnect(std::function<void()> cb);

private:
    boost::asio::awaitable<void> startAsync();
    boost::asio::awaitable<void> keepaliveLoop();
    boost::asio::awaitable<void> refreshListenKeyAfterReconnect();
    void startSessionWithCurrentListenKey();
    void onRawMessage(boost::system::error_code ec, std::string_view raw);

    boost::asio::io_context& m_ioc;
    boost::asio::ssl::context& m_ssl;
    RestClient m_rest;
    ContextConfig m_cfg;
    std::string m_listenKey;
    std::shared_ptr<WsSession> m_session;
    UserDataCb m_callback;
    boost::asio::steady_timer m_keepaliveTimer;
    std::function<void()> m_onDisconnect;
    std::function<void()> m_onReconnect;
    std::atomic<bool> m_refreshingListenKey{false};
};
