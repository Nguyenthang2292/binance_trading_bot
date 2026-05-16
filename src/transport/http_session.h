#pragma once

#include "context.h"
#include "common/expected_compat.h"
#include "types/error.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    using Result = std::expected<std::string, BinanceError>;

    HttpSession(boost::asio::io_context& ioc,
                boost::asio::ssl::context& ssl,
                std::string host,
                Socks5ProxyConfig proxy = {});

    boost::asio::awaitable<Result> get(std::string_view path,
                                       std::string_view query,
                                       std::string_view apiKey = "");
    boost::asio::awaitable<Result> post(std::string_view path,
                                        std::string_view body,
                                        std::string_view apiKey);
    boost::asio::awaitable<Result> put(std::string_view path,
                                       std::string_view body,
                                       std::string_view apiKey);
    boost::asio::awaitable<Result> del(std::string_view path,
                                       std::string_view query,
                                       std::string_view apiKey);

    int lastUsedWeight() const { return m_lastUsedWeight.load(); }
    int lastUsedOrders() const { return m_lastUsedOrders.load(); }
    int lastUsedOrders10s() const { return m_lastUsedOrders10s.load(); }

private:
    using Stream = boost::beast::ssl_stream<boost::beast::tcp_stream>;

    class RequestGate {
    public:
        class Lock {
        public:
            Lock() = default;
            explicit Lock(RequestGate& gate) : m_gate(&gate) {}
            Lock(const Lock&) = delete;
            Lock& operator=(const Lock&) = delete;
            Lock(Lock&& other) noexcept : m_gate(other.m_gate) { other.m_gate = nullptr; }
            Lock& operator=(Lock&& other) noexcept {
                if (this != &other) {
                    release();
                    m_gate = other.m_gate;
                    other.m_gate = nullptr;
                }
                return *this;
            }
            ~Lock() { release(); }

        private:
            void release();

            RequestGate* m_gate{nullptr};
        };

        boost::asio::awaitable<Lock> lock(boost::asio::io_context& ioc);

    private:
        friend class Lock;

        void unlock();

        std::mutex m_mutex;
        bool m_locked{false};
        std::deque<std::shared_ptr<boost::asio::steady_timer>> m_waiters;
    };

    boost::asio::awaitable<std::expected<void, BinanceError>> ensureConnected();
    boost::asio::awaitable<Result> execute(boost::beast::http::request<boost::beast::http::string_body> req);
    void resetStream();

    boost::asio::io_context& m_ioc;
    boost::asio::ssl::context& m_ssl;
    std::string m_host;
    Socks5ProxyConfig m_proxy;
    RequestGate m_requestGate;
    std::unique_ptr<Stream> m_stream;
    bool m_connected{false};
    std::atomic<int> m_lastUsedWeight{0};
    std::atomic<int> m_lastUsedOrders{0};
    std::atomic<int> m_lastUsedOrders10s{0};
};
