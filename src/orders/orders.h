#pragma once

#include "common/expected_compat.h"
#include "orders/irest_client.h"
#include "orders/order_id_generator.h"
#include "orders/order_journal.h"
#include "orders/order_mapper.h"
#include "orders/order_types.h"
#include "orders/order_validator.h"

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

template <typename T>
using OrdersResult = std::expected<T, BinanceError>;

class Orders {
public:
    Orders(IRestClient& rest, OrdersConfig cfg);

    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> market(MarketOrderDraft draft);
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> limit(LimitOrderDraft draft);
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> closeByMarket(CloseByMarketDraft draft);

    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelNormalByOrderId(Symbol symbol, int64_t orderId);
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelNormalByClientOrderId(
        Symbol symbol, ClientOrderId clientOrderId);
    boost::asio::awaitable<OrdersResult<void>> cancelAllNormal(Symbol symbol);

    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> queryNormalByOrderId(Symbol symbol, int64_t orderId);
    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> queryNormalByClientOrderId(
        Symbol symbol, ClientOrderId clientOrderId);
    boost::asio::awaitable<OrdersResult<std::vector<NormalOrderSnapshot>>> openNormalOrders(
        std::optional<Symbol> symbol = std::nullopt);
    boost::asio::awaitable<OrdersResult<std::vector<NormalOrderSnapshot>>> queryAllNormal(
        Symbol symbol,
        std::optional<int64_t> startTime = std::nullopt,
        std::optional<int64_t> endTime = std::nullopt,
        int limit = 500);

    boost::asio::awaitable<OrdersResult<BatchPlacementResult>> batchNormal(std::vector<NormalOrderDraft> drafts);

private:
    struct PreparedPlacement {
        OrderRequest request;
        NormalPlacementResult result;
    };

    IRestClient& m_rest;
    OrdersConfig m_cfg;
    OrderValidator m_validator;
    OrderMapper m_mapper;
    OrderIdGenerator m_idGenerator;
    std::shared_ptr<OrderJournal> m_journal;

    OrdersResult<PreparedPlacement> prepareMarket(MarketOrderDraft draft);
    OrdersResult<PreparedPlacement> prepareLimit(LimitOrderDraft draft);
    OrdersResult<PreparedPlacement> prepareCloseByMarket(CloseByMarketDraft draft);

    OrdersResult<ClientOrderId> resolveClientOrderId(const std::optional<ClientOrderId>& provided);
    std::expected<void, BinanceError> recordIntent(const PreparedPlacement& placement);
    std::expected<void, BinanceError> updateJournal(const CorrelationId& id,
                                                    PlacementState state,
                                                    std::optional<int64_t> orderId = std::nullopt);

    static NormalOrderSnapshot toSnapshot(const Order& order);
    static NormalCancelResult toCancelResult(const Order& order);
    static OrderErrorCategory mapErrorCategory(const BinanceError& error);
    static bool isAmbiguousPlacementError(const BinanceError& error);
    static int64_t unixMsNow();
    static std::optional<int> optionalCode(const BinanceError& error);
};
