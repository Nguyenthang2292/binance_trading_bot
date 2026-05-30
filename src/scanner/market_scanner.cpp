#include "scanner/market_scanner.h"

#include "logger.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace scanner {

namespace {

enum class WarmupPhase {
    Regular,
    Beta,
};

const char* phaseName(WarmupPhase phase) {
    return phase == WarmupPhase::Regular ? "klines" : "beta";
}

bool isTradableUsdtPerp(const ExchangeSymbol& symbol) {
    if (symbol.quoteAsset != "USDT") {
        return false;
    }
    if (symbol.contractType != "PERPETUAL") {
        return false;
    }
    return symbol.status == "TRADING";
}

bool shouldLogProgress(size_t completed, size_t total) {
    if (total == 0) {
        return false;
    }
    if (completed == 1 || completed == total) {
        return true;
    }
    return (completed % 25) == 0;
}

void logWarmupProgress(
    std::string_view phase,
    size_t completed,
    size_t total,
    std::string_view symbol,
    std::string_view interval) {
    if (!shouldLogProgress(completed, total)) {
        return;
    }

    const size_t percent = total == 0 ? 100 : (completed * 100 / total);
    Logger::instance().log(
        LogLevel::Info,
        "market_scanner warmup progress phase=" + std::string(phase) +
            " completed=" + std::to_string(completed) + "/" + std::to_string(total) +
            " (" + std::to_string(percent) + "%)" +
            " symbol=" + std::string(symbol) +
            " interval=" + std::string(interval));
}

class WarmupPool {
public:
    struct WorkItem {
        std::string symbol;
        std::string interval;
        int limit{99};
        WarmupPhase phase{WarmupPhase::Regular};
    };

    struct RunResult {
        size_t totalCompleted{0};
        size_t totalSucceeded{0};
        size_t totalFailed{0};
        size_t regularTotal{0};
        size_t regularSucceeded{0};
        size_t regularFailed{0};
        size_t betaTotal{0};
        size_t betaSucceeded{0};
        size_t betaFailed{0};
    };

    WarmupPool(BinanceContext& ctx, size_t concurrency, std::shared_ptr<RateLimiter> rateLimiter)
        : m_ctx(ctx), m_concurrency(std::max<size_t>(1, concurrency)), m_rateLimiter(std::move(rateLimiter)) {}

    boost::asio::awaitable<RunResult> run(std::vector<WorkItem> items, KlineCache& cache) {
        {
            std::lock_guard lock(m_mutex);
            m_queue = std::move(items);
            m_result = {};
            for (const auto& item : m_queue) {
                if (item.phase == WarmupPhase::Regular) {
                    ++m_result.regularTotal;
                } else {
                    ++m_result.betaTotal;
                }
            }
        }

        if (m_queue.empty()) {
            co_return m_result;
        }

        const size_t workerCount = std::min(m_concurrency, m_queue.size());
        std::vector<std::unique_ptr<RestClient>> clients;
        clients.reserve(workerCount);
        for (size_t i = 0; i < workerCount; ++i) {
            clients.push_back(std::make_unique<RestClient>(
                m_ctx.ioc(),
                m_ctx.sslContext(),
                m_ctx.config(),
                m_rateLimiter));
        }

        boost::asio::experimental::channel<void(boost::system::error_code)> done(m_ctx.ioc(), workerCount);
        for (size_t i = 0; i < workerCount; ++i) {
            boost::asio::co_spawn(
                m_ctx.ioc(),
                worker(*clients[i], cache),
                [&done](std::exception_ptr) {
                    done.try_send(boost::system::error_code{});
                });
        }

        for (size_t i = 0; i < workerCount; ++i) {
            co_await done.async_receive(boost::asio::use_awaitable);
        }

        co_return m_result;
    }

private:
    boost::asio::awaitable<void> worker(RestClient& client, KlineCache& cache) {
        while (true) {
            WorkItem item;
            size_t currentCompleted = 0;
            size_t currentTotal = 0;
            {
                std::lock_guard lock(m_mutex);
                if (m_queue.empty()) {
                    co_return;
                }
                item = std::move(m_queue.back());
                m_queue.pop_back();
                currentCompleted = m_result.totalCompleted;
                currentTotal = m_result.regularTotal + m_result.betaTotal;
            }
            (void)currentCompleted;
            (void)currentTotal;

            const auto klines = co_await client.klines(item.symbol, item.interval, item.limit);
            if (klines) {
                cache.merge(item.symbol, item.interval, *klines);
            }

            {
                std::lock_guard lock(m_mutex);
                ++m_result.totalCompleted;
                if (item.phase == WarmupPhase::Regular) {
                    if (klines) {
                        ++m_result.regularSucceeded;
                    } else {
                        ++m_result.regularFailed;
                    }
                } else {
                    if (klines) {
                        ++m_result.betaSucceeded;
                    } else {
                        ++m_result.betaFailed;
                    }
                }
                if (klines) {
                    ++m_result.totalSucceeded;
                } else {
                    ++m_result.totalFailed;
                    Logger::instance().log(
                        LogLevel::Warning,
                        "market_scanner warmup failed phase=" + std::string(phaseName(item.phase)) +
                            " symbol=" + item.symbol +
                            " interval=" + item.interval +
                            " limit=" + std::to_string(item.limit) +
                            " reason=" + klines.error().toString());
                }
                logWarmupProgress(
                    phaseName(item.phase),
                    m_result.totalCompleted,
                    m_result.regularTotal + m_result.betaTotal,
                    item.symbol,
                    item.interval);
            }
        }
    }

    BinanceContext& m_ctx;
    size_t m_concurrency;
    std::shared_ptr<RateLimiter> m_rateLimiter;

    std::mutex m_mutex;
    std::vector<WorkItem> m_queue;
    RunResult m_result;
};

} // namespace

MarketScanner::MarketScanner(RestClient& rest, BinanceContext& ctx, Config config)
    : m_rest(rest), m_ctx(ctx), m_config(std::move(config)), m_cache(m_config.klineBufferSize) {}

MarketScanner::~MarketScanner() {
    stop();
}

std::vector<std::string> MarketScanner::tradableUsdtPerpetualSymbols(const std::vector<ExchangeSymbol>& exchangeInfo) {
    std::vector<std::string> symbols;
    for (const auto& symbol : exchangeInfo) {
        if (isTradableUsdtPerp(symbol)) {
            symbols.push_back(symbol.symbol);
        }
    }
    return symbols;
}

size_t MarketScanner::streamConnectionCount(size_t symbolCount, size_t intervalCount, size_t maxStreamsPerConnection) {
    const size_t perConnection = std::max<size_t>(1, maxStreamsPerConnection);
    const size_t streams = symbolCount * intervalCount;
    return streams == 0 ? 0 : ((streams + perConnection - 1) / perConnection);
}

size_t MarketScanner::normalizedWarmupInitialLimit() const {
    const size_t cappedBuffer = std::max<size_t>(1, m_config.klineBufferSize);
    const size_t warmupLimit = std::max<size_t>(1, m_config.warmupInitialLimit);
    return std::min(warmupLimit, cappedBuffer);
}

boost::asio::awaitable<Result<void>> MarketScanner::waitForConnectionsReady(
    boost::asio::io_context& ioc,
    const std::vector<std::shared_ptr<std::atomic_bool>>& readyFlags,
    std::chrono::milliseconds timeout) {
    if (readyFlags.empty()) {
        co_return Result<void>{};
    }

    const auto effectiveTimeout = timeout <= std::chrono::milliseconds::zero()
        ? std::chrono::milliseconds{1}
        : timeout;
    const auto deadline = std::chrono::steady_clock::now() + effectiveTimeout;
    boost::asio::steady_timer timer(ioc);
    while (true) {
        const bool allReady = std::all_of(readyFlags.begin(), readyFlags.end(), [](const auto& flag) {
            return flag && flag->load();
        });
        if (allReady) {
            co_return Result<void>{};
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            co_return std::unexpected(BinanceError::fromApiResponse(
                -91004,
                "market scanner websocket feeds did not become healthy before timeout"));
        }

        timer.expires_after(std::chrono::milliseconds(100));
        boost::system::error_code ec;
        co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) {
            co_return std::unexpected(BinanceError::fromApiResponse(
                -91005,
                "market scanner startup wait interrupted"));
        }
    }
}

boost::asio::awaitable<Result<void>> MarketScanner::start() {
    stop();
    {
        std::lock_guard lock(m_stateMutex);
        m_symbolInfo.clear();
    }

    const auto warmupStartAt = std::chrono::steady_clock::now();

    const auto symbolsResult = co_await m_rest.exchangeInfo();
    if (!symbolsResult) {
        co_return std::unexpected(symbolsResult.error());
    }

    std::vector<std::string> symbols;
    for (const auto& symbol : *symbolsResult) {
        if (!isTradableUsdtPerp(symbol)) {
            continue;
        }
        symbols.push_back(symbol.symbol);
        std::lock_guard lock(m_stateMutex);
        m_symbolInfo[symbol.symbol] = symbol;
    }

    std::vector<WarmupPool::WorkItem> workItems;
    const size_t warmupInitialLimit = normalizedWarmupInitialLimit();
    workItems.reserve(symbols.size() * (m_config.intervals.size() + (m_config.betaDailyKlinesEnabled ? 1 : 0)));
    for (const auto& symbol : symbols) {
        for (const auto& interval : m_config.intervals) {
            workItems.push_back({
                .symbol = symbol,
                .interval = interval,
                .limit = static_cast<int>(warmupInitialLimit),
                .phase = WarmupPhase::Regular,
            });
        }
    }
    if (m_config.betaDailyKlinesEnabled) {
        std::vector<std::string> betaSymbols = symbols;
        if (std::find(betaSymbols.begin(), betaSymbols.end(), "BTCUSDT") == betaSymbols.end()) {
            betaSymbols.push_back("BTCUSDT");
        }
        const int betaLimit = std::max(2, m_config.betaDailyLimit);
        for (const auto& symbol : betaSymbols) {
            workItems.push_back({
                .symbol = symbol,
                .interval = m_config.betaDailyInterval,
                .limit = betaLimit,
                .phase = WarmupPhase::Beta,
            });
        }
    }

    Logger::instance().log(
        LogLevel::Info,
        "market_scanner warmup start symbols=" + std::to_string(symbols.size()) +
            " intervals=" + std::to_string(m_config.intervals.size()) +
            " requests=" + std::to_string(workItems.size()) +
            " concurrency=" + std::to_string(std::max<size_t>(1, m_config.warmupConcurrency)) +
            " initial_limit=" + std::to_string(warmupInitialLimit));

    WarmupPool pool(m_ctx, m_config.warmupConcurrency, m_ctx.rateLimiter());
    const auto warmupResult = co_await pool.run(std::move(workItems), m_cache);

    Logger::instance().log(
        LogLevel::Info,
        "market_scanner warmup phase complete phase=klines success=" +
            std::to_string(warmupResult.regularSucceeded) +
            " failed=" + std::to_string(warmupResult.regularFailed) +
            " total=" + std::to_string(warmupResult.regularTotal));
    if (m_config.betaDailyKlinesEnabled) {
        Logger::instance().log(
            LogLevel::Info,
            "market_scanner warmup phase complete phase=beta success=" +
                std::to_string(warmupResult.betaSucceeded) +
                " failed=" + std::to_string(warmupResult.betaFailed) +
                " total=" + std::to_string(warmupResult.betaTotal));
    }

    if (warmupResult.regularTotal > 0 && warmupResult.regularSucceeded == 0) {
        co_return std::unexpected(
            BinanceError::fromApiResponse(-91001, "market scanner warmup failed for all regular kline requests"));
    }

    auto subscribeResult = co_await subscribeStreams(symbols);
    if (!subscribeResult) {
        co_return std::unexpected(subscribeResult.error());
    }
    startBackfill(symbols);

    const auto warmupElapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - warmupStartAt);
    Logger::instance().log(
        LogLevel::Info,
        "market_scanner warmup complete elapsed_s=" + std::to_string(warmupElapsed.count()) +
            " ws_clients=" + std::to_string(m_wsClients.size()));
    co_return Result<void>{};
}

boost::asio::awaitable<Result<void>> MarketScanner::subscribeStreams(const std::vector<std::string>& symbols) {
    if (symbols.empty() || (m_config.intervals.empty() && !m_config.betaDailyKlinesEnabled)) {
        co_return Result<void>{};
    }

    const size_t perConnection = std::max<size_t>(1, m_config.maxStreamsPerConnection);
    std::vector<std::pair<std::string, std::string>> streams;
    streams.reserve(symbols.size() * (m_config.intervals.size() + (m_config.betaDailyKlinesEnabled ? 1 : 0)));
    for (const auto& symbol : symbols) {
        for (const auto& interval : m_config.intervals) {
            streams.emplace_back(symbol, interval);
        }
        if (m_config.betaDailyKlinesEnabled &&
            std::find(m_config.intervals.begin(), m_config.intervals.end(), m_config.betaDailyInterval) ==
                m_config.intervals.end()) {
            streams.emplace_back(symbol, m_config.betaDailyInterval);
        }
    }
    if (m_config.betaDailyKlinesEnabled) {
        const bool hasBtc = std::find(symbols.begin(), symbols.end(), "BTCUSDT") != symbols.end();
        if (!hasBtc) {
            streams.emplace_back("BTCUSDT", m_config.betaDailyInterval);
        }
    }

    Logger::instance().log(
        LogLevel::Info,
        "market_scanner subscribe start streams=" + std::to_string(streams.size()) +
            " max_per_connection=" + std::to_string(perConnection));

    std::vector<std::shared_ptr<std::atomic_bool>> readyFlags;
    readyFlags.reserve(streamConnectionCount(symbols.size(), m_config.intervals.size(), perConnection));

    size_t connectionIndex = 0;
    for (size_t i = 0; i < streams.size();) {
        auto ws = std::make_unique<WsClient>(m_ctx.ioc(), m_ctx.sslContext(), m_ctx.config());
        auto connectionReady = std::make_shared<std::atomic_bool>(false);
        readyFlags.push_back(connectionReady);
        ++connectionIndex;
        const size_t end = std::min(streams.size(), i + perConnection);
        Logger::instance().log(
            LogLevel::Info,
            "market_scanner subscribe connection=" + std::to_string(connectionIndex) +
                " streams=" + std::to_string(end - i));
        for (; i < end; ++i) {
            const auto [symbol, interval] = streams[i];
            ws->subscribeKline(symbol, interval, [this, connectionReady](boost::system::error_code ec, MarketEvent event) {
                if (ec) {
                    return;
                }
                if (const auto* kline = std::get_if<KlineEvent>(&event)) {
                    connectionReady->store(true);
                    m_cache.update(kline->symbol, kline->interval, kline->kline);
                    KlineClosedCb onKlineClosed;
                    {
                        std::lock_guard lock(m_stateMutex);
                        onKlineClosed = m_onKlineClosed;
                    }
                    if (kline->kline.isClosed && onKlineClosed) {
                        onKlineClosed(
                            kline->symbol,
                            kline->interval,
                            kline->kline.openTime,
                            kline->kline.closeTime);
                    }
                }
            });
        }
        ws->connect();
        m_wsClients.push_back(std::move(ws));
    }
    const auto readyTimeout = std::max(std::chrono::seconds(1), m_config.streamReadyTimeout);
    auto readyResult = co_await waitForConnectionsReady(
        m_ctx.ioc(),
        readyFlags,
        std::chrono::duration_cast<std::chrono::milliseconds>(readyTimeout));
    if (!readyResult) {
        co_return std::unexpected(readyResult.error());
    }
    Logger::instance().log(
        LogLevel::Info,
        "market_scanner subscribe complete ws_clients=" + std::to_string(m_wsClients.size()));

    co_return Result<void>{};
}

void MarketScanner::startBackfill(const std::vector<std::string>& symbols) {
    cancelBackfill();
    if (!m_config.backfillEnabled || symbols.empty() || m_config.intervals.empty()) {
        return;
    }
    if (m_config.klineBufferSize <= normalizedWarmupInitialLimit()) {
        return;
    }

    auto state = std::make_shared<BackfillState>();
    // Assign the future before publishing state so cancelBackfill() can always wait on it.
    state->done = boost::asio::co_spawn(
        m_ctx.ioc(),
        backgroundBackfill(std::vector<std::string>(symbols), state),
        boost::asio::use_future);
    {
        std::lock_guard lock(m_backfillMutex);
        m_backfillState = std::move(state);
    }
}

void MarketScanner::cancelBackfill() {
    std::shared_ptr<BackfillState> state;
    {
        std::lock_guard lock(m_backfillMutex);
        state = std::exchange(m_backfillState, nullptr);
    }
    if (!state) {
        return;
    }
    state->cancel.store(true);
    {
        std::lock_guard lock(state->mutex);
        if (state->timer) {
            boost::system::error_code ec;
            state->timer->cancel(ec);
        }
    }
    // Avoid waiting from an io_context worker thread. Blocking there can deadlock
    // the coroutine path needed to complete cancellation.
    if (m_ctx.ioc().get_executor().running_in_this_thread()) {
        return;
    }
    // Block until the coroutine exits when cancellation originates off the io threads.
    if (state->done.valid()) {
        try {
            state->done.wait();
        } catch (...) {
        }
    }
}

boost::asio::awaitable<void> MarketScanner::backgroundBackfill(
    std::vector<std::string> symbols,
    std::shared_ptr<BackfillState> state) {
    RestClient backfillRest(m_ctx.ioc(), m_ctx.sslContext(), m_ctx.config(), m_ctx.rateLimiter());
    const int fullLimit = static_cast<int>(std::max<size_t>(1, m_config.klineBufferSize));
    size_t completed = 0;
    const size_t total = symbols.size() * m_config.intervals.size();

    for (const auto& symbol : symbols) {
        for (const auto& interval : m_config.intervals) {
            if (state->cancel.load()) {
                co_return;
            }

            const auto klines = co_await backfillRest.klines(symbol, interval, fullLimit);
            if (state->cancel.load()) {
                co_return;
            }

            if (klines) {
                m_cache.merge(symbol, interval, *klines);
            } else {
                Logger::instance().log(
                    LogLevel::Warning,
                    "market_scanner backfill failed symbol=" + symbol +
                        " interval=" + interval +
                        " limit=" + std::to_string(fullLimit) +
                        " reason=" + klines.error().toString());
            }
            ++completed;

            if (m_config.backfillRequestDelay.count() > 0) {
                auto timer = std::make_shared<boost::asio::steady_timer>(m_ctx.ioc());
                {
                    std::lock_guard lock(state->mutex);
                    state->timer = timer;
                }
                timer->expires_after(m_config.backfillRequestDelay);
                boost::system::error_code ec;
                co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                {
                    std::lock_guard lock(state->mutex);
                    if (state->timer == timer) {
                        state->timer.reset();
                    }
                }
                if (ec || state->cancel.load()) {
                    co_return;
                }
            }
        }
    }

    Logger::instance().log(
        LogLevel::Info,
        "market_scanner backfill complete completed=" + std::to_string(completed) +
            "/" + std::to_string(total));
}

void MarketScanner::stop() {
    cancelBackfill();
    {
        std::lock_guard lock(m_stateMutex);
        m_onKlineClosed = {};
    }
    for (auto& ws : m_wsClients) {
        ws->disconnect();
    }
    m_wsClients.clear();
}

std::vector<std::string> MarketScanner::symbols() const {
    auto symbols = m_cache.symbols();
    if (!symbols.empty()) {
        return symbols;
    }
    std::lock_guard lock(m_stateMutex);
    symbols.reserve(m_symbolInfo.size());
    for (const auto& [symbol, _] : m_symbolInfo) {
        symbols.push_back(symbol);
    }
    return symbols;
}

std::optional<ExchangeSymbol> MarketScanner::symbolInfo(std::string_view symbol) const {
    std::lock_guard lock(m_stateMutex);
    const auto it = m_symbolInfo.find(std::string(symbol));
    if (it == m_symbolInfo.end()) {
        return std::nullopt;
    }
    return it->second;
}

void MarketScanner::setOnKlineClosed(KlineClosedCb cb) {
    std::lock_guard lock(m_stateMutex);
    m_onKlineClosed = std::move(cb);
}

} // namespace scanner
