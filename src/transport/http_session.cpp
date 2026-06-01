#include "transport/http_session.h"

#include "transport/rate_limit_headers.h"
#include "transport/socks5_proxy.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/version.hpp>
#include <openssl/ssl.h>

#include <algorithm>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

HttpSession::HttpSession(asio::io_context& ioc,
                         ssl::context& sslContext,
                         std::string host,
                         Socks5ProxyConfig proxy)
    : m_ioc(ioc), m_ssl(sslContext), m_host(std::move(host)), m_proxy(std::move(proxy)) {
    resetStream();
}

void HttpSession::RequestGate::Lock::release() {
    if (!m_gate) {
        return;
    }
    m_gate->unlock();
    m_gate = nullptr;
}

asio::awaitable<HttpSession::RequestGate::Lock> HttpSession::RequestGate::lock(asio::io_context& ioc) {
    auto waiter = std::make_shared<Waiter>(ioc);

    bool acquired = false;
    {
        std::lock_guard guard(m_mutex);
        if (!m_locked) {
            m_locked = true;
            acquired = true;
        } else {
            m_waiters.push_back(waiter);
        }
    }

    if (!acquired) {
        while (true) {
            boost::system::error_code ec;
            co_await waiter->timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

            bool granted = false;
            {
                std::lock_guard guard(m_mutex);
                granted = waiter->granted;
                if (!granted) {
                    auto it = std::find(m_waiters.begin(), m_waiters.end(), waiter);
                    if (it != m_waiters.end()) {
                        m_waiters.erase(it);
                    }
                }
            }
            if (granted) {
                break;
            }
            throw boost::system::system_error(
                ec ? ec : boost::asio::error::operation_aborted);
        }
    }

    co_return Lock(*this);
}

void HttpSession::RequestGate::unlock() {
    std::shared_ptr<Waiter> next;
    {
        std::lock_guard guard(m_mutex);
        while (!m_waiters.empty()) {
            next = m_waiters.front();
            m_waiters.pop_front();
            if (next) {
                break;
            }
        }
        if (!next) {
            m_locked = false;
            return;
        }
        next->granted = true;
    }

    boost::system::error_code ec;
    next->timer.cancel(ec);
}

void HttpSession::resetStream() {
    m_stream = std::make_unique<Stream>(m_ioc, m_ssl);
    m_connected = false;
}

asio::awaitable<compat::expected<void, BinanceError>> HttpSession::ensureConnected() {
    if (m_connected) {
        co_return compat::expected<void, BinanceError>{};
    }

    try {
        resetStream();
        m_stream->set_verify_mode(ssl::verify_peer);
        m_stream->set_verify_callback(ssl::host_name_verification(m_host));
        if (!SSL_set_tlsext_host_name(m_stream->native_handle(), m_host.c_str())) {
            co_return compat::unexpected(BinanceError{ErrorCategory::Network, 0, "failed to set SNI host name"});
        }

        const std::string connectHost = m_proxy.enabled() ? m_proxy.host : m_host;
        const std::string connectPort = m_proxy.enabled() ? std::to_string(m_proxy.port) : "443";

        tcp::resolver resolver(m_ioc);
        auto results = co_await resolver.async_resolve(connectHost, connectPort, asio::use_awaitable);
        beast::get_lowest_layer(*m_stream).expires_after(std::chrono::seconds(30));
        co_await beast::get_lowest_layer(*m_stream).async_connect(results, asio::use_awaitable);
        if (m_proxy.enabled()) {
            co_await transport::socks5::connectTunnel(
                beast::get_lowest_layer(*m_stream).socket(),
                m_host,
                443);
        }
        co_await m_stream->async_handshake(ssl::stream_base::client, asio::use_awaitable);
        beast::get_lowest_layer(*m_stream).expires_never();
        m_connected = true;
        co_return compat::expected<void, BinanceError>{};
    } catch (const boost::system::system_error& e) {
        resetStream();
            co_return compat::unexpected(BinanceError::fromNetwork(e.code(), NetworkErrorPhase::BeforeSend));
    } catch (const std::exception& e) {
        resetStream();
        co_return compat::unexpected(BinanceError{ErrorCategory::Network, 0, e.what()});
    }
}

asio::awaitable<HttpSession::Result> HttpSession::get(std::string_view path,
                                                      std::string_view query,
                                                      std::string_view apiKey) {
    std::string target(path);
    if (!query.empty()) {
        target += '?';
        target += query;
    }
    http::request<http::string_body> req{http::verb::get, target, 11};
    if (!apiKey.empty()) {
        req.set("X-MBX-APIKEY", apiKey);
    }
    co_return co_await execute(std::move(req));
}

asio::awaitable<HttpSession::Result> HttpSession::post(std::string_view path,
                                                       std::string_view body,
                                                       std::string_view apiKey) {
    http::request<http::string_body> req{http::verb::post, std::string(path), 11};
    req.body() = std::string(body);
    req.prepare_payload();
    req.set(http::field::content_type, "application/x-www-form-urlencoded");
    if (!apiKey.empty()) {
        req.set("X-MBX-APIKEY", apiKey);
    }
    co_return co_await execute(std::move(req));
}

asio::awaitable<HttpSession::Result> HttpSession::put(std::string_view path,
                                                      std::string_view body,
                                                      std::string_view apiKey) {
    http::request<http::string_body> req{http::verb::put, std::string(path), 11};
    req.body() = std::string(body);
    req.prepare_payload();
    req.set(http::field::content_type, "application/x-www-form-urlencoded");
    if (!apiKey.empty()) {
        req.set("X-MBX-APIKEY", apiKey);
    }
    co_return co_await execute(std::move(req));
}

asio::awaitable<HttpSession::Result> HttpSession::del(std::string_view path,
                                                      std::string_view query,
                                                      std::string_view apiKey) {
    std::string target(path);
    if (!query.empty()) {
        target += '?';
        target += query;
    }
    http::request<http::string_body> req{http::verb::delete_, target, 11};
    if (!apiKey.empty()) {
        req.set("X-MBX-APIKEY", apiKey);
    }
    co_return co_await execute(std::move(req));
}

asio::awaitable<HttpSession::Result> HttpSession::execute(http::request<http::string_body> req) {
    // Beast streams allow only one in-flight operation chain per connection.
    auto requestLock = co_await m_requestGate.lock(m_ioc);

    if (auto connected = co_await ensureConnected(); !connected) {
        co_return compat::unexpected(connected.error());
    }

    req.set(http::field::host, m_host);
    req.set(http::field::user_agent, "binance-futures-sdk-cpp/1.0");
    req.keep_alive(true);

    bool requestWritten = false;
    try {
        http::response<http::string_body> res;
        beast::flat_buffer buffer;
        beast::get_lowest_layer(*m_stream).expires_after(m_requestIoTimeout);
        co_await http::async_write(*m_stream, req, asio::use_awaitable);
        requestWritten = true;
        beast::get_lowest_layer(*m_stream).expires_after(m_requestIoTimeout);
        co_await http::async_read(*m_stream, buffer, res, asio::use_awaitable);
        beast::get_lowest_layer(*m_stream).expires_never();

        RateLimitHeaderUsage usage;
        for (const auto& field : res) {
            applyRateLimitHeader(usage, field.name_string(), field.value());
        }
        if (usage.usedWeight1m >= 0) {
            m_lastUsedWeight = usage.usedWeight1m;
        }
        if (usage.usedOrders1m >= 0) {
            m_lastUsedOrders = usage.usedOrders1m;
        }
        if (usage.usedOrders10s >= 0) {
            m_lastUsedOrders10s = usage.usedOrders10s;
        }

        if (!res.keep_alive()) {
            resetStream();
        }

        const int status = static_cast<int>(res.result_int());
        if (status >= 400) {
            co_return compat::unexpected(BinanceError::fromHttp(status, res.body()));
        }
        co_return res.body();
    } catch (const boost::system::system_error& e) {
        resetStream();
        co_return compat::unexpected(BinanceError::fromNetwork(
            e.code(),
            requestWritten ? NetworkErrorPhase::AfterSend : NetworkErrorPhase::BeforeSend));
    } catch (const std::exception& e) {
        resetStream();
        co_return compat::unexpected(BinanceError{ErrorCategory::Network, 0, e.what()});
    }
}
