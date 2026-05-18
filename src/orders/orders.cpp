#include "orders/orders.h"

Orders::Orders(IRestClient& rest, OrdersConfig cfg) {
    m_normalService = std::make_unique<NormalOrderService>(rest, cfg);
    m_algoService = std::make_unique<AlgoOrderService>(rest, std::move(cfg));
}

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> Orders::market(MarketOrderDraft draft) {
    co_return co_await m_normalService->market(std::move(draft));
}

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> Orders::limit(LimitOrderDraft draft) {
    co_return co_await m_normalService->limit(std::move(draft));
}

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> Orders::closeByMarket(CloseByMarketDraft draft) {
    co_return co_await m_normalService->closeByMarket(std::move(draft));
}

boost::asio::awaitable<OrdersResult<LeverageResult>> Orders::setLeverage(Symbol symbol, int leverage) {
    co_return co_await m_normalService->setLeverage(std::move(symbol), leverage);
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> Orders::amendLimitOrder(AmendLimitOrderDraft draft) {
    co_return co_await m_normalService->amendLimitOrder(std::move(draft));
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> Orders::amendLimitOrderByOrderId(
    Symbol symbol,
    OrderSide side,
    int64_t orderId,
    Quantity quantity,
    Price price,
    std::optional<ResponseType> responseType,
    std::optional<int64_t> recvWindow) {
    co_return co_await m_normalService->amendLimitOrderByOrderId(
        std::move(symbol), side, orderId, std::move(quantity), std::move(price), responseType, recvWindow);
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> Orders::amendLimitOrderByClientOrderId(
    Symbol symbol,
    OrderSide side,
    ClientOrderId clientOrderId,
    Quantity quantity,
    Price price,
    std::optional<ResponseType> responseType,
    std::optional<int64_t> recvWindow) {
    co_return co_await m_normalService->amendLimitOrderByClientOrderId(
        std::move(symbol), side, std::move(clientOrderId), std::move(quantity), std::move(price), responseType, recvWindow);
}

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> Orders::stopEntry(StopEntryDraft draft) {
    co_return co_await m_algoService->stopEntry(std::move(draft));
}

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> Orders::protection(ProtectionOrderDraft draft) {
    co_return co_await m_algoService->protection(std::move(draft));
}

boost::asio::awaitable<OrdersResult<NormalCancelResult>> Orders::cancelNormalByOrderId(Symbol symbol, int64_t orderId) {
    co_return co_await m_normalService->cancelNormalByOrderId(std::move(symbol), orderId);
}

boost::asio::awaitable<OrdersResult<NormalCancelResult>> Orders::cancelNormalByClientOrderId(
    Symbol symbol,
    ClientOrderId clientOrderId) {
    co_return co_await m_normalService->cancelNormalByClientOrderId(std::move(symbol), std::move(clientOrderId));
}

boost::asio::awaitable<OrdersResult<void>> Orders::cancelAllNormal(Symbol symbol) {
    co_return co_await m_normalService->cancelAllNormal(std::move(symbol));
}

boost::asio::awaitable<OrdersResult<NormalCancelResult>> Orders::cancelAlgoByAlgoId(Symbol symbol, int64_t algoId) {
    co_return co_await m_algoService->cancelAlgoByAlgoId(std::move(symbol), algoId);
}

boost::asio::awaitable<OrdersResult<NormalCancelResult>> Orders::cancelAlgoByClientAlgoId(
    Symbol symbol,
    ClientAlgoId clientAlgoId) {
    co_return co_await m_algoService->cancelAlgoByClientAlgoId(std::move(symbol), std::move(clientAlgoId));
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> Orders::queryNormalByOrderId(Symbol symbol, int64_t orderId) {
    co_return co_await m_normalService->queryNormalByOrderId(std::move(symbol), orderId);
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> Orders::queryNormalByClientOrderId(
    Symbol symbol,
    ClientOrderId clientOrderId) {
    co_return co_await m_normalService->queryNormalByClientOrderId(std::move(symbol), std::move(clientOrderId));
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> Orders::queryAlgoByAlgoId(Symbol symbol, int64_t algoId) {
    co_return co_await m_algoService->queryAlgoByAlgoId(std::move(symbol), algoId);
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> Orders::queryAlgoByClientAlgoId(
    Symbol symbol,
    ClientAlgoId clientAlgoId) {
    co_return co_await m_algoService->queryAlgoByClientAlgoId(std::move(symbol), std::move(clientAlgoId));
}

boost::asio::awaitable<OrdersResult<std::vector<NormalOrderSnapshot>>> Orders::openNormalOrders(std::optional<Symbol> symbol) {
    co_return co_await m_normalService->openNormalOrders(std::move(symbol));
}

boost::asio::awaitable<OrdersResult<std::vector<NormalOrderSnapshot>>> Orders::queryAllNormal(
    Symbol symbol,
    std::optional<int64_t> startTime,
    std::optional<int64_t> endTime,
    int limit) {
    co_return co_await m_normalService->queryAllNormal(std::move(symbol), startTime, endTime, limit);
}

boost::asio::awaitable<OrdersResult<OrderFillSummary>> Orders::queryOrderFillSummary(Symbol symbol, int64_t orderId) {
    co_return co_await m_normalService->queryOrderFillSummary(std::move(symbol), orderId);
}

boost::asio::awaitable<OrdersResult<OrderPoolSnapshot>> Orders::openNormalOrderSnapshot(
    std::optional<Symbol> symbol) {
    co_return co_await m_normalService->openNormalOrderSnapshot(std::move(symbol));
}

boost::asio::awaitable<OrdersResult<OrderPoolSnapshot>> Orders::queryAllNormalSnapshot(
    Symbol symbol,
    std::optional<int64_t> startTime,
    std::optional<int64_t> endTime,
    int limit) {
    co_return co_await m_normalService->queryAllNormalSnapshot(std::move(symbol), startTime, endTime, limit);
}

boost::asio::awaitable<OrdersResult<BatchPlacementResult>> Orders::batchNormal(std::vector<NormalOrderDraft> drafts) {
    co_return co_await m_normalService->batchNormal(std::move(drafts));
}
