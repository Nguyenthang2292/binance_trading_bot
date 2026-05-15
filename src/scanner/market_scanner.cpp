#include "scanner/market_scanner.h"

#include "logger.h"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>

namespace scanner {

namespace {

bool isTradableUsdtPerp(const ExchangeSymbol& symbol) {
    if (symbol.quoteAsset != "USDT") {
        return false;
    }
    if (symbol.contractType != "PERPETUAL") {
        return false;
    }
    return symbol.status == "TRADING";
}

} // namespace

MarketScanner::MarketScanner(RestClient& rest, BinanceContext& ctx, Config config)
    : m_rest(rest), m_ctx(ctx), m_config(std::move(config)), m_cache(m_config.klineBufferSize) {}

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

boost::asio::awaitable<Result<void>> MarketScanner::start() {
    stop();

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
        m_symbolInfo[symbol.symbol] = symbol;
    }

    for (const auto& symbol : symbols) {
        for (const auto& interval : m_config.intervals) {
            const auto klines = co_await m_rest.klines(symbol, interval, static_cast<int>(m_config.klineBufferSize));
            if (!klines) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "market_scanner warmup failed symbol=" + symbol + " interval=" + interval +
                        " reason=" + klines.error().toString());
                continue;
            }
            for (const auto& kline : *klines) {
                m_cache.update(symbol, interval, kline);
            }
            if (m_config.warmupRequestDelay.count() > 0) {
                boost::asio::steady_timer timer(m_ctx.ioc());
                timer.expires_after(m_config.warmupRequestDelay);
                boost::system::error_code ec;
                co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                if (ec) {
                    co_return std::unexpected(BinanceError::fromNetwork(ec));
                }
            }
        }
    }

    if (m_config.betaDailyKlinesEnabled) {
        std::vector<std::string> betaSymbols = symbols;
        if (std::find(betaSymbols.begin(), betaSymbols.end(), "BTCUSDT") == betaSymbols.end()) {
            betaSymbols.push_back("BTCUSDT");
        }
        const int betaLimit = std::max(2, m_config.betaDailyLimit);
        for (const auto& symbol : betaSymbols) {
            const auto klines = co_await m_rest.klines(symbol, m_config.betaDailyInterval, betaLimit);
            if (!klines) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "market_scanner beta warmup failed symbol=" + symbol + " interval=" + m_config.betaDailyInterval +
                        " reason=" + klines.error().toString());
                continue;
            }
            for (const auto& kline : *klines) {
                m_cache.update(symbol, m_config.betaDailyInterval, kline);
            }
            if (m_config.warmupRequestDelay.count() > 0) {
                boost::asio::steady_timer timer(m_ctx.ioc());
                timer.expires_after(m_config.warmupRequestDelay);
                boost::system::error_code ec;
                co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                if (ec) {
                    co_return std::unexpected(BinanceError::fromNetwork(ec));
                }
            }
        }
    }

    co_await subscribeStreams(symbols);
    co_return Result<void>{};
}

boost::asio::awaitable<void> MarketScanner::subscribeStreams(const std::vector<std::string>& symbols) {
    if (symbols.empty() || (m_config.intervals.empty() && !m_config.betaDailyKlinesEnabled)) {
        co_return;
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

    for (size_t i = 0; i < streams.size();) {
        auto ws = std::make_unique<WsClient>(m_ctx.ioc(), m_ctx.sslContext(), m_ctx.config());
        const size_t end = std::min(streams.size(), i + perConnection);
        for (; i < end; ++i) {
            const auto [symbol, interval] = streams[i];
            ws->subscribeKline(symbol, interval, [this](boost::system::error_code ec, MarketEvent event) {
                if (ec) {
                    return;
                }
                if (const auto* kline = std::get_if<KlineEvent>(&event)) {
                    m_cache.update(kline->symbol, kline->interval, kline->kline);
                    if (kline->kline.isClosed && m_onKlineClosed) {
                        m_onKlineClosed(kline->symbol, kline->interval);
                    }
                }
            });
        }
        ws->connect();
        m_wsClients.push_back(std::move(ws));
    }

    co_return;
}

void MarketScanner::stop() {
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
    symbols.reserve(m_symbolInfo.size());
    for (const auto& [symbol, _] : m_symbolInfo) {
        symbols.push_back(symbol);
    }
    return symbols;
}

std::optional<ExchangeSymbol> MarketScanner::symbolInfo(std::string_view symbol) const {
    const auto it = m_symbolInfo.find(std::string(symbol));
    if (it == m_symbolInfo.end()) {
        return std::nullopt;
    }
    return it->second;
}

void MarketScanner::setOnKlineClosed(KlineClosedCb cb) {
    m_onKlineClosed = std::move(cb);
}

} // namespace scanner
