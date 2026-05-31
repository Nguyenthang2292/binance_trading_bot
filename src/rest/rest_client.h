#pragma once

#include "common/expected_compat.h"
#include "context.h"
#include "rest/rate_limiter.h"
#include "rest/signer.h"
#include "transport/http_session.h"
#include "types/account.h"
#include "types/events.h"
#include "types/market.h"
#include "types/trade.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <simdjson.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

template <typename T>
using Result = std::expected<T, BinanceError>;

class RestClient {
public:
    RestClient(boost::asio::io_context& ioc, boost::asio::ssl::context& ssl, ContextConfig cfg);
    RestClient(boost::asio::io_context& ioc,
               boost::asio::ssl::context& ssl,
               ContextConfig cfg,
               std::shared_ptr<RateLimiter> sharedRateLimiter);

    boost::asio::awaitable<Result<bool>> ping();
    boost::asio::awaitable<Result<int64_t>> serverTime();
    boost::asio::awaitable<Result<std::vector<ExchangeSymbol>>> exchangeInfo();
    boost::asio::awaitable<Result<OrderBook>> orderBook(std::string symbol, int limit = 20);
    boost::asio::awaitable<Result<std::vector<Kline>>> klines(std::string symbol,
                                                              std::string interval,
                                                              int limit = 500,
                                                              std::optional<int64_t> startTime = {},
                                                              std::optional<int64_t> endTime = {});
    boost::asio::awaitable<Result<MarkPrice>> markPrice(std::string symbol);
    boost::asio::awaitable<Result<std::vector<MarkPrice>>> allMarkPrices();
    boost::asio::awaitable<Result<double>> fundingRate(std::string symbol);
    boost::asio::awaitable<Result<Ticker24h>> ticker24h(std::string symbol);
    boost::asio::awaitable<Result<std::vector<Ticker24h>>> allTicker24h();
    boost::asio::awaitable<Result<double>> bestBidPrice(std::string symbol);

    boost::asio::awaitable<Result<FuturesAccount>> account();
    boost::asio::awaitable<Result<std::vector<Balance>>> balance();
    boost::asio::awaitable<Result<std::vector<Position>>> positions(std::optional<std::string> symbol = {});
    boost::asio::awaitable<Result<FuturesAccountConfig>> accountConfig();
    boost::asio::awaitable<Result<PositionModeStatus>> positionMode();
    boost::asio::awaitable<Result<MultiAssetsModeStatus>> multiAssetsMode();
    boost::asio::awaitable<Result<void>> testOrder(OrderRequest req);
    boost::asio::awaitable<Result<std::vector<SymbolLeverageBrackets>>> leverageBrackets(
        std::optional<std::string> symbol = {});
    boost::asio::awaitable<Result<LeverageResult>> setLeverage(std::string symbol, int leverage);
    boost::asio::awaitable<Result<void>> setMarginType(std::string symbol, std::string marginType);

    boost::asio::awaitable<Result<Order>> newOrder(OrderRequest req);
    boost::asio::awaitable<Result<Order>> newAlgoOrder(OrderRequest req);
    boost::asio::awaitable<Result<Order>> modifyOrder(OrderRequest req);
    boost::asio::awaitable<Result<Order>> cancelOrder(std::string symbol, int64_t orderId);
    boost::asio::awaitable<Result<Order>> cancelAlgoOrder(std::string symbol, int64_t algoId);
    boost::asio::awaitable<Result<Order>> cancelOrderByClientOrderId(std::string symbol, std::string clientOrderId);
    boost::asio::awaitable<Result<Order>> cancelAlgoOrderByClientAlgoId(
        std::string symbol,
        std::string clientAlgoId);
    boost::asio::awaitable<Result<void>> cancelAllOrders(std::string symbol);
    boost::asio::awaitable<Result<Order>> queryOrder(std::string symbol, int64_t orderId);
    boost::asio::awaitable<Result<Order>> queryAlgoOrder(std::string symbol, int64_t algoId);
    boost::asio::awaitable<Result<Order>> queryOrderByClientOrderId(std::string symbol, std::string clientOrderId);
    boost::asio::awaitable<Result<Order>> queryAlgoOrderByClientAlgoId(
        std::string symbol,
        std::string clientAlgoId);
    boost::asio::awaitable<Result<std::vector<Order>>> openOrders(std::optional<std::string> symbol = {});
    boost::asio::awaitable<Result<std::vector<Order>>> allOrders(std::string symbol,
                                                                 std::optional<int64_t> startTime = {},
                                                                 std::optional<int64_t> endTime = {},
                                                                 int limit = 500);
    boost::asio::awaitable<Result<std::vector<UserTrade>>> userTrades(std::string symbol,
                                                                  std::optional<int64_t> orderId = {},
                                                                  std::optional<int64_t> startTime = {},
                                                                  std::optional<int64_t> endTime = {},
                                                                  int limit = 500);
    boost::asio::awaitable<Result<BatchOrderResult>> batchOrders(std::vector<OrderRequest> reqs);

    boost::asio::awaitable<Result<std::string>> createListenKey();
    boost::asio::awaitable<Result<void>> keepAliveListenKey(std::string listenKey);
    boost::asio::awaitable<Result<void>> deleteListenKey(std::string listenKey);

    struct RawParseStorage {
        simdjson::padded_string buffer;
        simdjson::ondemand::parser parser;
    };

    struct RawParsedDocument {
        std::shared_ptr<RawParseStorage> storage;
        simdjson::ondemand::document document;
        std::string_view body;

        RawParsedDocument(
            std::shared_ptr<RawParseStorage> owned,
            simdjson::ondemand::document&& doc,
            std::string_view bodyView)
            : storage(std::move(owned)), document(std::move(doc)), body(bodyView) {}
        RawParsedDocument(RawParsedDocument&&) noexcept = default;
        RawParsedDocument& operator=(RawParsedDocument&&) noexcept = default;
        RawParsedDocument(const RawParsedDocument&) = delete;
        RawParsedDocument& operator=(const RawParsedDocument&) = delete;
    };

    using RawParseResult = std::expected<RawParsedDocument, BinanceError>;

    template <typename T>
    Result<T> parseResponse(std::string_view body,
                            std::function<T(simdjson::ondemand::document&)> parser);
    RawParseResult rawParse(std::string_view body);

private:
    std::shared_ptr<HttpSession> m_session;
    Signer m_signer;
    std::shared_ptr<RateLimiter> m_rateLimiter;
    ContextConfig m_cfg;

    boost::asio::awaitable<HttpSession::Result> publicGet(
        std::string_view path,
        std::string query,
        RateLimiter::Cost cost = {});
    boost::asio::awaitable<HttpSession::Result> signedGet(
        std::string_view path,
        std::string params,
        RateLimiter::Cost cost = {});
    boost::asio::awaitable<HttpSession::Result> signedPost(
        std::string_view path,
        std::string params,
        RateLimiter::Cost cost = {});
    boost::asio::awaitable<HttpSession::Result> signedPut(
        std::string_view path,
        std::string params,
        RateLimiter::Cost cost = {});
    boost::asio::awaitable<HttpSession::Result> signedDelete(
        std::string_view path,
        std::string params,
        RateLimiter::Cost cost = {});
    boost::asio::awaitable<HttpSession::Result> apiKeyPost(
        std::string_view path,
        std::string params,
        RateLimiter::Cost cost = {});
    boost::asio::awaitable<HttpSession::Result> apiKeyPut(
        std::string_view path,
        std::string params,
        RateLimiter::Cost cost = {});
    boost::asio::awaitable<HttpSession::Result> apiKeyDelete(
        std::string_view path,
        std::string params,
        RateLimiter::Cost cost = {});

};

template <typename T>
Result<T> RestClient::parseResponse(std::string_view body,
                                    std::function<T(simdjson::ondemand::document&)> parser) {
    auto parsed = rawParse(body);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    try {
        return parser(parsed->document);
    } catch (const std::exception& e) {
        return std::unexpected(BinanceError::fromParse(e.what()));
    }
}
