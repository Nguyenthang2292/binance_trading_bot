#pragma once

#include "context.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

using WsMessageCb = std::function<void(boost::system::error_code, std::string_view)>;
using WsSimpleCb = std::function<void()>;

struct ReconnectConfig {
    std::chrono::milliseconds initialBackoff{1000};
    std::chrono::milliseconds maxBackoff{30000};
    double backoffMultiplier{2.0};
};

class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    WsSession(boost::asio::io_context& ioc,
              boost::asio::ssl::context& ssl,
              std::string host,
              Socks5ProxyConfig proxy = {},
              ReconnectConfig cfg = {});

    void start(std::string path,
               WsMessageCb onMessage,
               WsSimpleCb onDisconnect = {},
               WsSimpleCb onReconnect = {});
    void stop();
    void send(std::string message);

private:
    using WebSocket = boost::beast::websocket::stream<
        boost::beast::ssl_stream<boost::beast::tcp_stream>>;

    boost::asio::awaitable<void> connectLoop();
    boost::asio::awaitable<void> doConnect();
    boost::asio::awaitable<void> readLoop();
    void resetSocket();

    boost::asio::io_context& m_ioc;
    boost::asio::ssl::context& m_ssl;
    std::string m_host;
    Socks5ProxyConfig m_proxy;
    std::string m_path;
    std::unique_ptr<WebSocket> m_ws;
    WsMessageCb m_onMessage;
    WsSimpleCb m_onDisconnect;
    WsSimpleCb m_onReconnect;
    ReconnectConfig m_reconnectCfg;
    std::atomic<bool> m_stopped{false};
    std::atomic<bool> m_connected{false};
    std::chrono::steady_clock::time_point m_connectedAt;
};
