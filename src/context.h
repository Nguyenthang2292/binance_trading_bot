#pragma once

#include "rest/signer.h"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class RestClient;
class WsClient;
class UserDataStream;

namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;

struct Socks5ProxyConfig {
    std::string host;
    std::uint16_t port = 0;

    bool enabled() const { return !host.empty() && port != 0; }
};

struct ContextConfig {
    std::string apiKey;
    std::string secretKey;
    bool testnet = false;
    size_t threadPoolSize = 2;
    SigningMethod signingMethod = SigningMethod::HMAC_SHA256;
    Socks5ProxyConfig socks5Proxy;
};

class BinanceContext {
public:
    explicit BinanceContext(ContextConfig cfg);
    ~BinanceContext();

    BinanceContext(const BinanceContext&) = delete;
    BinanceContext& operator=(const BinanceContext&) = delete;

    RestClient makeRestClient();
    WsClient makeWsClient();
    UserDataStream makeUserDataStream();

    asio::io_context& ioc() { return m_ioc; }
    ssl::context& sslContext() { return m_ssl; }
    const ContextConfig& config() const { return m_cfg; }

private:
    ContextConfig m_cfg;
    asio::io_context m_ioc;
    asio::executor_work_guard<asio::io_context::executor_type> m_work;
    ssl::context m_ssl;
    std::vector<std::thread> m_threads;
};
