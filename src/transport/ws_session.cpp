#include "transport/ws_session.h"

#include "transport/socks5_proxy.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <openssl/ssl.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = boost::beast::websocket;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

WsSession::WsSession(asio::io_context& ioc,
                     ssl::context& sslContext,
                     std::string host,
                     Socks5ProxyConfig proxy,
                     ReconnectConfig cfg)
    : m_ioc(ioc),
      m_ssl(sslContext),
      m_host(std::move(host)),
      m_proxy(std::move(proxy)),
      m_reconnectCfg(cfg) {
    resetSocket();
}

void WsSession::resetSocket() {
    m_ws = std::make_unique<WebSocket>(m_ioc, m_ssl);
    m_connected = false;
}

void WsSession::start(std::string path, WsMessageCb onMessage, WsSimpleCb onDisconnect, WsSimpleCb onReconnect) {
    m_path = std::move(path);
    m_onMessage = std::move(onMessage);
    m_onDisconnect = std::move(onDisconnect);
    m_onReconnect = std::move(onReconnect);
    m_stopped = false;

    auto self = shared_from_this();
    asio::co_spawn(m_ioc, [self] { return self->connectLoop(); }, asio::detached);
}

void WsSession::stop() {
    m_stopped = true;
    auto self = shared_from_this();
    asio::post(m_ioc, [self] {
        if (self->m_ws && self->m_connected) {
            boost::system::error_code ec;
            self->m_ws->close(websocket::close_code::normal, ec);
        }
        self->resetSocket();
    });
}

void WsSession::send(std::string message) {
    auto self = shared_from_this();
    asio::post(m_ioc, [self, message = std::move(message)] {
        if (!self->m_ws || !self->m_connected) {
            return;
        }
        self->m_ws->text(true);
        self->m_ws->async_write(asio::buffer(message),
                                [self](boost::system::error_code, std::size_t) {});
    });
}

asio::awaitable<void> WsSession::connectLoop() {
    auto delay = m_reconnectCfg.initialBackoff;
    bool firstConnect = true;

    while (!m_stopped) {
        try {
            co_await doConnect();
            m_connected = true;
            m_connectedAt = std::chrono::steady_clock::now();
            if (!firstConnect && m_onReconnect) {
                m_onReconnect();
            }
            firstConnect = false;
            delay = m_reconnectCfg.initialBackoff;

            co_await readLoop();
        } catch (const boost::system::system_error& e) {
            if (m_onMessage) {
                m_onMessage(e.code(), {});
            }
        } catch (...) {
            if (m_onMessage) {
                m_onMessage(asio::error::connection_reset, {});
            }
        }

        if (m_stopped) {
            break;
        }

        m_connected = false;
        resetSocket();
        if (m_onDisconnect) {
            m_onDisconnect();
        }

        asio::steady_timer timer(m_ioc);
        timer.expires_after(delay);
        co_await timer.async_wait(asio::use_awaitable);

        const auto nextMs = static_cast<int64_t>(delay.count() * m_reconnectCfg.backoffMultiplier);
        delay = std::min(std::chrono::milliseconds(nextMs), m_reconnectCfg.maxBackoff);
    }
}

asio::awaitable<void> WsSession::doConnect() {
    resetSocket();
    if (!SSL_set_tlsext_host_name(m_ws->next_layer().native_handle(), m_host.c_str())) {
        throw boost::system::system_error(asio::error::operation_aborted);
    }

    const std::string connectHost = m_proxy.enabled() ? m_proxy.host : m_host;
    const std::string connectPort = m_proxy.enabled() ? std::to_string(m_proxy.port) : "443";

    tcp::resolver resolver(m_ioc);
    auto results = co_await resolver.async_resolve(connectHost, connectPort, asio::use_awaitable);
    beast::get_lowest_layer(*m_ws).expires_after(std::chrono::seconds(30));
    co_await beast::get_lowest_layer(*m_ws).async_connect(results, asio::use_awaitable);
    if (m_proxy.enabled()) {
        co_await transport::socks5::connectTunnel(
            beast::get_lowest_layer(*m_ws).socket(),
            m_host,
            443);
    }
    co_await m_ws->next_layer().async_handshake(ssl::stream_base::client, asio::use_awaitable);
    beast::get_lowest_layer(*m_ws).expires_never();

    m_ws->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    m_ws->set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(boost::beast::http::field::user_agent, "binance-futures-sdk-cpp/1.0");
    }));
    co_await m_ws->async_handshake(m_host, m_path, asio::use_awaitable);
}

asio::awaitable<void> WsSession::readLoop() {
    beast::flat_buffer buffer;
    while (!m_stopped) {
        buffer.clear();
        co_await m_ws->async_read(buffer, asio::use_awaitable);
        const auto text = beast::buffers_to_string(buffer.data());
        if (m_onMessage) {
            m_onMessage({}, text);
        }

        if (std::chrono::steady_clock::now() - m_connectedAt >= std::chrono::hours(23) + std::chrono::minutes(50)) {
            boost::system::error_code ec;
            m_ws->close(websocket::close_code::normal, ec);
            throw boost::system::system_error(asio::error::connection_reset);
        }
    }
}
