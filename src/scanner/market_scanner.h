#pragma once

#include "context.h"
#include "rest/rest_client.h"
#include "scanner/kline_cache.h"
#include "ws/ws_client.h"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace scanner {

class MarketScanner {
public:
    struct Config {
        std::vector<std::string> intervals{"15m", "30m"};
        size_t klineBufferSize{200};
        size_t maxStreamsPerConnection{512};
        std::chrono::milliseconds warmupRequestDelay{100};
    };

    MarketScanner(RestClient& rest, BinanceContext& ctx, Config config);

    boost::asio::awaitable<Result<void>> start();
    void stop();

    const KlineCache& cache() const { return m_cache; }
    std::vector<std::string> symbols() const;
    std::optional<ExchangeSymbol> symbolInfo(std::string_view symbol) const;
    boost::asio::io_context& ioContext() { return m_ctx.ioc(); }

    static std::vector<std::string> tradableUsdtPerpetualSymbols(const std::vector<ExchangeSymbol>& exchangeInfo);
    static size_t streamConnectionCount(size_t symbolCount, size_t intervalCount, size_t maxStreamsPerConnection);

    using KlineClosedCb = std::function<void(std::string_view symbol, std::string_view interval)>;
    void setOnKlineClosed(KlineClosedCb cb);

private:
    boost::asio::awaitable<void> subscribeStreams(const std::vector<std::string>& symbols);

    RestClient& m_rest;
    BinanceContext& m_ctx;
    Config m_config;
    KlineCache m_cache;
    std::vector<std::unique_ptr<WsClient>> m_wsClients;
    KlineClosedCb m_onKlineClosed;
    std::unordered_map<std::string, ExchangeSymbol> m_symbolInfo;
};

} // namespace scanner
