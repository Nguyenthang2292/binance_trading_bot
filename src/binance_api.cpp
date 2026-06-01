#include "binance_api.h"

#include "context.h"
#include "logger.h"
#include "orders/orders.h"
#include "orders/rest_client_adapter.h"
#include "rest/rest_client.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <future>

OrdersConfig BinanceAPI::makeLegacyOrdersConfig() {
    OrdersConfig cfg;
    cfg.clientIdNamespace = "legacy";
    cfg.allowBestEffortJournal = false;
    cfg.defaultResponseType = ResponseType::RESULT;
    cfg.recvWindow = std::chrono::milliseconds{5000};
    cfg.allowRawTimestampOverride = false;
    cfg.positionMode = PositionMode::OneWay;
    cfg.journalIsDurable = true;
    cfg.journalPath = "data/legacy_orders_journal.log";
    return cfg;
}

BinanceAPI::BinanceAPI(const std::string& apiKey, const std::string& secretKey)
    : m_context(std::make_unique<BinanceContext>([&] {
          ContextConfig cfg;
          cfg.apiKey = apiKey;
          cfg.secretKey = secretKey;
          cfg.testnet = false;
          cfg.threadPoolSize = 2;
          return cfg;
      }())),
      m_rest(std::make_unique<RestClient>(m_context->ioc(), m_context->sslContext(), m_context->config())),
      m_ordersAdapter(std::make_unique<RestClientAdapter>(*m_rest)) {
    OrdersConfig ordersConfig = makeLegacyOrdersConfig();
    m_orders = std::make_unique<Orders>(*m_ordersAdapter, std::move(ordersConfig));

    Logger::instance().log(LogLevel::Info, "Binance Futures API initialized");
}

BinanceAPI::~BinanceAPI() = default;

std::optional<double> BinanceAPI::getPrice(const std::string& symbol) {
    auto future = boost::asio::co_spawn(
        m_context->ioc(),
        [this, symbol]() -> boost::asio::awaitable<Result<Ticker24h>> {
            co_return co_await m_rest->ticker24h(symbol);
        },
        boost::asio::use_future);
    auto result = future.get();
    if (!result) {
        Logger::instance().log(LogLevel::Error, "Failed to get futures price: " + result.error().toString());
        return std::nullopt;
    }
    return result->lastPrice;
}

std::vector<Kline> BinanceAPI::getKlines(const std::string& symbol, const std::string& interval, int limit) {
    auto future = boost::asio::co_spawn(
        m_context->ioc(),
        [this, symbol, interval, limit]() -> boost::asio::awaitable<Result<std::vector<Kline>>> {
            co_return co_await m_rest->klines(symbol, interval, limit);
        },
        boost::asio::use_future);
    auto result = future.get();
    if (!result) {
        Logger::instance().log(LogLevel::Error, "Failed to get futures klines: " + result.error().toString());
        return {};
    }
    return *result;
}

std::vector<Ticker> BinanceAPI::get24hrTickers() {
    auto future = boost::asio::co_spawn(
        m_context->ioc(),
        [this]() -> boost::asio::awaitable<Result<std::vector<Ticker24h>>> {
            co_return co_await m_rest->allTicker24h();
        },
        boost::asio::use_future);
    auto result = future.get();
    if (!result) {
        Logger::instance().log(LogLevel::Error, "Failed to get futures tickers: " + result.error().toString());
        return {};
    }
    return *result;
}

std::optional<AccountInfo> BinanceAPI::getAccountInfo() {
    auto future = boost::asio::co_spawn(
        m_context->ioc(),
        [this]() -> boost::asio::awaitable<Result<std::vector<Balance>>> {
            co_return co_await m_rest->balance();
        },
        boost::asio::use_future);
    auto result = future.get();
    if (!result) {
        Logger::instance().log(LogLevel::Error, "Failed to get futures balance: " + result.error().toString());
        return std::nullopt;
    }

    AccountInfo info;
    for (const auto& balance : *result) {
        const double total = balance.walletBalance;
        if (total > 0.0) {
            info.balances[balance.asset] = total;
            info.totalBalance += total;
        }
    }
    return info;
}

std::optional<std::vector<Position>> BinanceAPI::getPositions(const std::string& symbol) {
    auto future = boost::asio::co_spawn(
        m_context->ioc(),
        [this, symbol]() -> boost::asio::awaitable<Result<std::vector<Position>>> {
            co_return co_await m_rest->positions(symbol);
        },
        boost::asio::use_future);
    auto result = future.get();
    if (!result) {
        Logger::instance().log(LogLevel::Error, "Failed to get futures positions: " + result.error().toString());
        return std::nullopt;
    }
    return *result;
}

bool BinanceAPI::testConnectivity() {
    auto future = boost::asio::co_spawn(
        m_context->ioc(),
        [this]() -> boost::asio::awaitable<Result<bool>> {
            co_return co_await m_rest->ping();
        },
        boost::asio::use_future);
    auto result = future.get();
    if (!result) {
        Logger::instance().log(LogLevel::Error, "Futures ping failed: " + result.error().toString());
        return false;
    }
    return *result;
}

compat::expected<NormalPlacementResult, BinanceError> BinanceAPI::marketOrder(MarketOrderDraft draft) {
    auto future = boost::asio::co_spawn(
        m_context->ioc(),
        [this, draft = std::move(draft)]() mutable -> boost::asio::awaitable<OrdersResult<NormalPlacementResult>> {
            co_return co_await m_orders->market(std::move(draft));
        },
        boost::asio::use_future);
    return future.get();
}

compat::expected<NormalPlacementResult, BinanceError> BinanceAPI::limitOrder(LimitOrderDraft draft) {
    auto future = boost::asio::co_spawn(
        m_context->ioc(),
        [this, draft = std::move(draft)]() mutable -> boost::asio::awaitable<OrdersResult<NormalPlacementResult>> {
            co_return co_await m_orders->limit(std::move(draft));
        },
        boost::asio::use_future);
    return future.get();
}

compat::expected<NormalPlacementResult, BinanceError> BinanceAPI::closeByMarket(CloseByMarketDraft draft) {
    auto future = boost::asio::co_spawn(
        m_context->ioc(),
        [this, draft = std::move(draft)]() mutable -> boost::asio::awaitable<OrdersResult<NormalPlacementResult>> {
            co_return co_await m_orders->closeByMarket(std::move(draft));
        },
        boost::asio::use_future);
    return future.get();
}

compat::expected<NormalCancelResult, BinanceError> BinanceAPI::cancelNormalByOrderId(const Symbol& symbol, int64_t orderId) {
    auto future = boost::asio::co_spawn(
        m_context->ioc(),
        [this, symbol, orderId]() -> boost::asio::awaitable<OrdersResult<NormalCancelResult>> {
            co_return co_await m_orders->cancelNormalByOrderId(symbol, orderId);
        },
        boost::asio::use_future);
    return future.get();
}

compat::expected<NormalOrderSnapshot, BinanceError> BinanceAPI::queryNormalByOrderId(const Symbol& symbol, int64_t orderId) {
    auto future = boost::asio::co_spawn(
        m_context->ioc(),
        [this, symbol, orderId]() -> boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> {
            co_return co_await m_orders->queryNormalByOrderId(symbol, orderId);
        },
        boost::asio::use_future);
    return future.get();
}
