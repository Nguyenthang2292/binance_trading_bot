#include "context.h"

#include "logger.h"
#include "rest/rate_limiter.h"
#include "rest/rest_client.h"
#include "ws/user_data_stream.h"
#include "ws/ws_client.h"

#include <algorithm>
#include <exception>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace {

size_t loadWindowsRootCertificates(ssl::context& context) {
    HCERTSTORE systemStore = CertOpenSystemStoreA(0, "ROOT");
    if (!systemStore) {
        Logger::instance().log(LogLevel::Warning, "Failed to open Windows ROOT certificate store");
        return 0;
    }

    X509_STORE* opensslStore = SSL_CTX_get_cert_store(context.native_handle());
    size_t loaded = 0;

    PCCERT_CONTEXT certContext = nullptr;
    while ((certContext = CertEnumCertificatesInStore(systemStore, certContext)) != nullptr) {
        const unsigned char* encoded = certContext->pbCertEncoded;
        X509* cert = d2i_X509(nullptr, &encoded, static_cast<long>(certContext->cbCertEncoded));
        if (!cert) {
            ERR_clear_error();
            continue;
        }

        if (X509_STORE_add_cert(opensslStore, cert) == 1) {
            ++loaded;
        } else {
            ERR_clear_error();
        }
        X509_free(cert);
    }

    CertCloseStore(systemStore, 0);
    return loaded;
}

} // namespace
#endif

BinanceContext::BinanceContext(ContextConfig cfg)
    : m_cfg(std::move(cfg)),
      m_rateLimiter(std::make_shared<RateLimiter>()),
      m_work(asio::make_work_guard(m_ioc)),
      m_ssl(ssl::context::tls_client) {
    m_ssl.set_default_verify_paths();
#ifdef _WIN32
    const size_t loadedRootCertificates = loadWindowsRootCertificates(m_ssl);
    Logger::instance().log(LogLevel::Info,
                           "Loaded Windows root certificates into OpenSSL store: "
                               + std::to_string(loadedRootCertificates));
#endif
    m_ssl.set_verify_mode(ssl::verify_peer);

    const size_t threadCount = std::max<size_t>(1, m_cfg.threadPoolSize);
    m_threads.reserve(threadCount);
    for (size_t i = 0; i < threadCount; ++i) {
        m_threads.emplace_back([this] {
            while (!m_ioc.stopped()) {
                try {
                    m_ioc.run();
                    break;
                } catch (const std::exception& e) {
                    Logger::instance().log(LogLevel::Error, std::string("io_context thread exception: ") + e.what());
                } catch (...) {
                    Logger::instance().log(LogLevel::Error, "io_context thread unknown exception");
                }
            }
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
    return RestClient(m_ioc, m_ssl, m_cfg, m_rateLimiter);
}

WsClient BinanceContext::makeWsClient() {
    return WsClient(m_ioc, m_ssl, m_cfg);
}

UserDataStream BinanceContext::makeUserDataStream() {
    return UserDataStream(m_ioc, m_ssl, m_cfg);
}
