#include "transport/ws_session.h"

#include "logger.h"
#include "transport/socks5_proxy.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <openssl/ssl.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <string>
#include <utility>

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
      m_strand(asio::make_strand(ioc)),
      m_host(std::move(host)),
      m_proxy(std::move(proxy)),
      m_reconnectCfg(cfg) {
    resetSocket();
}

void WsSession::resetSocket() {
    m_ws = std::make_unique<WebSocket>(m_strand, m_ssl);
    m_connected = false;
}

void WsSession::clearWriteQueue() {
    std::queue<std::string> empty;
    m_outboundMessages.swap(empty);
}

void WsSession::start(std::string path, WsMessageCb onMessage, WsSimpleCb onDisconnect, WsSimpleCb onReconnect) {
    auto self = shared_from_this();
    asio::dispatch(
        m_strand,
        [self,
         path = std::move(path),
         onMessage = std::move(onMessage),
         onDisconnect = std::move(onDisconnect),
         onReconnect = std::move(onReconnect)]() mutable {
            self->m_path = std::move(path);
            self->m_onMessage = std::move(onMessage);
            self->m_onDisconnect = std::move(onDisconnect);
            self->m_onReconnect = std::move(onReconnect);
            self->m_stopped = false;
            self->clearWriteQueue();
            self->m_writerRunning = false;

            asio::co_spawn(
                self->m_strand,
                [self] { return self->connectLoop(); },
                [](std::exception_ptr ep) {
                    if (!ep) {
                        return;
                    }
                    try {
                        std::rethrow_exception(ep);
                    } catch (const std::exception& e) {
                        Logger::instance().log(
                            LogLevel::Error,
                            std::string("WsSession connectLoop exception: ") + e.what());
                    } catch (...) {
                        Logger::instance().log(LogLevel::Error, "WsSession connectLoop unknown exception");
                    }
                });
        });
}

void WsSession::stop() {
    auto self = shared_from_this();
    asio::post(m_strand, [self] {
        self->m_stopped = true;
        self->clearWriteQueue();
        self->m_writerRunning = false;

        if (!self->m_ws) {
            return;
        }

        boost::system::error_code ec;
        beast::get_lowest_layer(*self->m_ws).socket().cancel(ec);
        if (self->m_ws->is_open()) {
            self->m_ws->async_close(websocket::close_code::normal, [](boost::system::error_code) {});
        }
    });
}

void WsSession::send(std::string message) {
    auto self = shared_from_this();
    asio::post(m_strand, [self, message = std::move(message)]() mutable {
        if (self->m_stopped) {
            return;
        }
        self->m_outboundMessages.push(std::move(message));
        if (!self->m_connected || self->m_writerRunning) {
            return;
        }
        self->m_writerRunning = true;
        asio::co_spawn(
            self->m_strand,
            [self] { return self->writeLoop(); },
            [self](std::exception_ptr ep) {
                self->m_writerRunning = false;
                if (!ep) {
                    return;
                }
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    if (self->m_onMessage) {
                        self->m_onMessage(boost::asio::error::operation_aborted, std::string{});
                    }
                    Logger::instance().log(
                        LogLevel::Warning,
                        std::string("WsSession writeLoop exception: ") + e.what());
                } catch (...) {
                    if (self->m_onMessage) {
                        self->m_onMessage(boost::asio::error::operation_aborted, std::string{});
                    }
                    Logger::instance().log(LogLevel::Warning, "WsSession writeLoop unknown exception");
                }
            });
    });
}

void WsSession::onConnected() {
    if (m_connected && !m_outboundMessages.empty() && !m_writerRunning) {
        m_writerRunning = true;
        auto self = shared_from_this();
        asio::co_spawn(
            m_strand,
            [self] { return self->writeLoop(); },
            [self](std::exception_ptr ep) {
                self->m_writerRunning = false;
                if (!ep) {
                    return;
                }
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    if (self->m_onMessage) {
                        self->m_onMessage(boost::asio::error::operation_aborted, std::string{});
                    }
                    Logger::instance().log(
                        LogLevel::Warning,
                        std::string("WsSession writeLoop exception: ") + e.what());
                } catch (...) {
                    if (self->m_onMessage) {
                        self->m_onMessage(boost::asio::error::operation_aborted, std::string{});
                    }
                    Logger::instance().log(LogLevel::Warning, "WsSession writeLoop unknown exception");
                }
            });
    }
}

asio::awaitable<void> WsSession::connectLoop() {
    auto delay = m_reconnectCfg.initialBackoff;
    bool firstConnect = true;

    while (!m_stopped) {
        try {
            co_await doConnect();
            m_connected = true;
            m_connectedAt = std::chrono::steady_clock::now();
            onConnected();
            if (!firstConnect && m_onReconnect) {
                m_onReconnect();
            }
            firstConnect = false;
            delay = m_reconnectCfg.initialBackoff;

            co_await readLoop();
        } catch (const boost::system::system_error& e) {
            if (m_onMessage) {
                m_onMessage(e.code(), std::string{});
            }
        } catch (...) {
            if (m_onMessage) {
                m_onMessage(asio::error::connection_reset, std::string{});
            }
        }

        if (m_stopped) {
            break;
        }

        m_connected = false;
        m_writerRunning = false;
        clearWriteQueue();
        resetSocket();
        if (m_onDisconnect) {
            m_onDisconnect();
        }

        asio::steady_timer timer(m_strand);
        timer.expires_after(delay);
        co_await timer.async_wait(asio::use_awaitable);

        const auto nextMs = static_cast<int64_t>(delay.count() * m_reconnectCfg.backoffMultiplier);
        delay = std::min(std::chrono::milliseconds(nextMs), m_reconnectCfg.maxBackoff);
    }
}

asio::awaitable<void> WsSession::doConnect() {
    resetSocket();
    m_ws->next_layer().set_verify_mode(ssl::verify_peer);
    m_ws->next_layer().set_verify_callback(ssl::host_name_verification(m_host));
    if (!SSL_set_tlsext_host_name(m_ws->next_layer().native_handle(), m_host.c_str())) {
        throw boost::system::system_error(asio::error::operation_aborted);
    }

    const std::string connectHost = m_proxy.enabled() ? m_proxy.host : m_host;
    const std::string connectPort = m_proxy.enabled() ? std::to_string(m_proxy.port) : "443";

    tcp::resolver resolver(m_strand);
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

asio::awaitable<void> WsSession::writeLoop() {
    while (!m_stopped && m_connected && m_ws && !m_outboundMessages.empty()) {
        std::string message = std::move(m_outboundMessages.front());
        m_outboundMessages.pop();
        m_ws->text(true);
        co_await m_ws->async_write(asio::buffer(message), asio::use_awaitable);
    }
    m_writerRunning = false;
}

asio::awaitable<void> WsSession::readLoop() {
    beast::flat_buffer buffer;
    while (!m_stopped) {
        buffer.clear();
        co_await m_ws->async_read(buffer, asio::use_awaitable);
        std::string text = beast::buffers_to_string(buffer.data());
        if (m_onMessage) {
            m_onMessage({}, std::move(text));
        }

        if (std::chrono::steady_clock::now() - m_connectedAt >= std::chrono::hours(23) + std::chrono::minutes(50)) {
            boost::system::error_code ec;
            m_ws->close(websocket::close_code::normal, ec);
            throw boost::system::system_error(asio::error::connection_reset);
        }
    }
}
