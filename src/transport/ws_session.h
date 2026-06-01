#pragma once

#include "context.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <utility>

using WsMessageCb = std::function<void(boost::system::error_code, std::string)>;
using WsSimpleCb = std::function<void()>;

struct ReconnectConfig {
    std::chrono::milliseconds initialBackoff{1000};
    std::chrono::milliseconds maxBackoff{30000};
    double backoffMultiplier{2.0};
};

class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    /** Creates a reconnecting websocket session serialized on a strand. */
    WsSession(boost::asio::io_context& ioc,
              boost::asio::ssl::context& ssl,
              std::string host,
              Socks5ProxyConfig proxy = {},
              ReconnectConfig cfg = {});

    /** Starts the connect/read loop with user callbacks. */
    void start(std::string path,
               WsMessageCb onMessage,
               WsSimpleCb onDisconnect = {},
               WsSimpleCb onReconnect = {});
    /** Requests graceful shutdown and cancels pending async operations. */
    void stop();
    /** Queues a text frame for serialized async send. */
    void send(std::string message);

private:
    using WebSocket = boost::beast::websocket::stream<
        boost::beast::ssl_stream<boost::beast::tcp_stream>>;

    boost::asio::awaitable<void> connectLoop();
    boost::asio::awaitable<void> doConnect();
    boost::asio::awaitable<void> readLoop();
    boost::asio::awaitable<void> writeLoop();
    void onConnected();
    void armReconnectDeadline();
    void cancelReconnectDeadline();
    void clearWriteQueue();
    void resetSocket();

    boost::asio::io_context& m_ioc;
    boost::asio::ssl::context& m_ssl;
    boost::asio::strand<boost::asio::io_context::executor_type> m_strand;
    std::string m_host;
    Socks5ProxyConfig m_proxy;
    std::string m_path;
    std::unique_ptr<WebSocket> m_ws;
    WsMessageCb m_onMessage;
    WsSimpleCb m_onDisconnect;
    WsSimpleCb m_onReconnect;
    ReconnectConfig m_reconnectCfg;
    std::unique_ptr<boost::asio::steady_timer> m_reconnectDeadlineTimer;
    std::atomic<bool> m_stopped{false};
    std::atomic<bool> m_connected{false};
    bool m_writerRunning{false};
    std::queue<std::string> m_outboundMessages;
    std::chrono::steady_clock::time_point m_connectedAt;
    std::chrono::milliseconds m_maxSessionLifetime{std::chrono::hours(23) + std::chrono::minutes(50)};
};
