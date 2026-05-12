#include "context.h"

#include "rest/rest_client.h"
#include "ws/user_data_stream.h"
#include "ws/ws_client.h"

#include <algorithm>

BinanceContext::BinanceContext(ContextConfig cfg)
    : m_cfg(std::move(cfg)),
      m_work(asio::make_work_guard(m_ioc)),
      m_ssl(ssl::context::tls_client) {
    m_ssl.set_default_verify_paths();
    m_ssl.set_verify_mode(ssl::verify_peer);

    const size_t threadCount = std::max<size_t>(1, m_cfg.threadPoolSize);
    m_threads.reserve(threadCount);
    for (size_t i = 0; i < threadCount; ++i) {
        m_threads.emplace_back([this] {
            m_ioc.run();
        });
    }
}

BinanceContext::~BinanceContext() {
    m_work.reset();
    m_ioc.stop();
    for (auto& thread : m_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

RestClient BinanceContext::makeRestClient() {
    return RestClient(m_ioc, m_ssl, m_cfg);
}

WsClient BinanceContext::makeWsClient() {
    return WsClient(m_ioc, m_ssl, m_cfg);
}

UserDataStream BinanceContext::makeUserDataStream() {
    return UserDataStream(m_ioc, m_ssl, m_cfg);
}
