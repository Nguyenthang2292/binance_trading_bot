#pragma once

#include "orders/algo_order_service.h"
#include "orders/irest_client.h"
#include "orders/normal_order_service.h"
#include "orders/order_types.h"

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

class Orders {
public:
    Orders(IRestClient& rest, OrdersConfig cfg);

    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> market(MarketOrderDraft draft);
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> limit(LimitOrderDraft draft);
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> closeByMarket(CloseByMarketDraft draft);
    boost::asio::awaitable<OrdersResult<LeverageResult>> setLeverage(Symbol symbol, int leverage);
    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> amendLimitOrder(AmendLimitOrderDraft draft);

    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> amendLimitOrderByOrderId(
        Symbol symbol,
        OrderSide side,
        int64_t orderId,
        Quantity quantity,
        Price price,
        std::optional<ResponseType> responseType = std::nullopt,
        std::optional<int64_t> recvWindow = std::nullopt);

    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> amendLimitOrderByClientOrderId(
        Symbol symbol,
        OrderSide side,
        ClientOrderId clientOrderId,
        Quantity quantity,
        Price price,
        std::optional<ResponseType> responseType = std::nullopt,
        std::optional<int64_t> recvWindow = std::nullopt);

    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> stopEntry(StopEntryDraft draft);
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> protection(ProtectionOrderDraft draft);

    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelNormalByOrderId(Symbol symbol, int64_t orderId);
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelNormalByClientOrderId(
        Symbol symbol, ClientOrderId clientOrderId);
    boost::asio::awaitable<OrdersResult<void>> cancelAllNormal(Symbol symbol);

    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelAlgoByAlgoId(Symbol symbol, int64_t algoId);
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelAlgoByClientAlgoId(
        Symbol symbol, ClientAlgoId clientAlgoId);

    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> queryNormalByOrderId(Symbol symbol, int64_t orderId);
    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> queryNormalByClientOrderId(
        Symbol symbol, ClientOrderId clientOrderId);

    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> queryAlgoByAlgoId(Symbol symbol, int64_t algoId);
    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> queryAlgoByClientAlgoId(
        Symbol symbol, ClientAlgoId clientAlgoId);
    boost::asio::awaitable<OrdersResult<std::vector<NormalOrderSnapshot>>> openNormalOrders(
        std::optional<Symbol> symbol = std::nullopt);
    boost::asio::awaitable<OrdersResult<std::vector<NormalOrderSnapshot>>> queryAllNormal(
        Symbol symbol,
        std::optional<int64_t> startTime = std::nullopt,
        std::optional<int64_t> endTime = std::nullopt,
        int limit = 500);

    boost::asio::awaitable<OrdersResult<OrderFillSummary>> queryOrderFillSummary(Symbol symbol, int64_t orderId);

    boost::asio::awaitable<OrdersResult<OrderPoolSnapshot>> openNormalOrderSnapshot(
        std::optional<Symbol> symbol = std::nullopt);
    boost::asio::awaitable<OrdersResult<OrderPoolSnapshot>> queryAllNormalSnapshot(
        Symbol symbol,
        std::optional<int64_t> startTime = std::nullopt,
        std::optional<int64_t> endTime = std::nullopt,
        int limit = 500);

    boost::asio::awaitable<OrdersResult<BatchPlacementResult>> batchNormal(std::vector<NormalOrderDraft> drafts);

private:
    std::unique_ptr<NormalOrderService> m_normalService;
    std::unique_ptr<AlgoOrderService> m_algoService;
};
