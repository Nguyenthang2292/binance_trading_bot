#pragma once

#include "context.h"
#include "rest/rest_client.h"
#include "scanner/kline_cache.h"
#include "ws/ws_client.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
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
        std::chrono::milliseconds warmupRequestDelay{0};
        size_t warmupInitialLimit{99};
        size_t warmupConcurrency{10};
        bool backfillEnabled{true};
        size_t backfillConcurrency{1};
        std::chrono::milliseconds backfillRequestDelay{200};
        bool betaDailyKlinesEnabled{false};
        std::string betaDailyInterval{"1d"};
        int betaDailyLimit{31};
    };

    MarketScanner(RestClient& rest, BinanceContext& ctx, Config config);
    ~MarketScanner();

    boost::asio::awaitable<Result<void>> start();
    void stop();

    const KlineCache& cache() const { return m_cache; }
    std::vector<std::string> symbols() const;
    std::optional<ExchangeSymbol> symbolInfo(std::string_view symbol) const;
    boost::asio::io_context& ioContext() { return m_ctx.ioc(); }

    static std::vector<std::string> tradableUsdtPerpetualSymbols(const std::vector<ExchangeSymbol>& exchangeInfo);
    static size_t streamConnectionCount(size_t symbolCount, size_t intervalCount, size_t maxStreamsPerConnection);

    using KlineClosedCb = std::function<void(
        std::string_view symbol,
        std::string_view interval,
        int64_t openTimeMs,
        int64_t closeTimeMs)>;
    void setOnKlineClosed(KlineClosedCb cb);

private:
    struct BackfillState {
        std::atomic_bool cancel{false};
        std::mutex mutex;
        std::shared_ptr<boost::asio::steady_timer> timer;
        std::future<void> done;
    };

    boost::asio::awaitable<void> subscribeStreams(const std::vector<std::string>& symbols);
    boost::asio::awaitable<void> backgroundBackfill(
        std::vector<std::string> symbols,
        std::shared_ptr<BackfillState> state);
    void startBackfill(const std::vector<std::string>& symbols);
    void cancelBackfill();
    size_t normalizedWarmupInitialLimit() const;

    RestClient& m_rest;
    BinanceContext& m_ctx;
    Config m_config;
    KlineCache m_cache;
    std::vector<std::unique_ptr<WsClient>> m_wsClients;
    KlineClosedCb m_onKlineClosed;
    std::unordered_map<std::string, ExchangeSymbol> m_symbolInfo;
    std::mutex m_backfillMutex;
    std::shared_ptr<BackfillState> m_backfillState;
};

} // namespace scanner
