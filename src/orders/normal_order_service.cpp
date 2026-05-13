#include "orders/normal_order_service.h"
#include "orders/order_service_utils.h"

#include "logger.h"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace {
using orders::detail::addValidationIssue;
using orders::detail::attachErrorDetails;
using orders::detail::errorCategoryToString;
using orders::detail::isAmbiguousPlacementError;
using orders::detail::mapErrorCategory;
using orders::detail::stateToString;
using orders::detail::typeToString;

std::atomic<int64_t> g_unknownPendingReconcileCount{0};

std::string sideToString(OrderSide side) {
    return side == OrderSide::Buy ? "BUY" : "SELL";
}

std::string positionSideToString(PositionSide side) {
    switch (side) {
        case PositionSide::Both: return "BOTH";
        case PositionSide::Long: return "LONG";
        case PositionSide::Short: return "SHORT";
    }
    return "BOTH";
}

std::string tifToString(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::GTX: return "GTX";
    }
    return "GTC";
}

std::string boolToString(bool value) {
    return value ? "true" : "false";
}

void logPlacement(const NormalPlacementResult& result, std::chrono::steady_clock::time_point startedAt) {
    if (result.state == PlacementState::UnknownPendingReconcile) {
        ++g_unknownPendingReconcileCount;
    }

    const auto latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt).count();

    std::ostringstream out;
    out << "order_placement"
        << " correlationId=" << result.correlationId
        << " clientOrderId=" << result.clientOrderId
        << " symbol=" << result.symbol
        << " endpoint=" << result.endpoint.value_or("")
        << " latencyMs=" << latencyMs
        << " state=" << stateToString(result.state)
        << " errorCategory=" << errorCategoryToString(result.errorCategory)
        << " binanceCode=" << result.binanceCode.value_or(0)
        << " unknownPendingReconcileCount=" << g_unknownPendingReconcileCount.load();
    Logger::instance().log(LogLevel::Info, out.str());
}

void appendParam(std::string& out, std::string_view key, std::string_view value) {
    if (value.empty()) {
        return;
    }
    if (!out.empty()) {
        out.push_back('&');
    }
    out.append(key);
    out.push_back('=');
    out.append(value);
}

using Decimal50 = long double;

bool tryParseDecimal(std::string_view text, Decimal50& out) {
    try {
        out = std::stold(std::string(text));
        return true;
    } catch (...) {
        return false;
    }
}

std::string formatDecimal(const Decimal50& value) {
    std::ostringstream out;
    out.setf(std::ios::fixed, std::ios::floatfield);
    out << std::setprecision(std::numeric_limits<Decimal50>::digits10) << value;
    std::string text = out.str();

    const auto dotPos = text.find('.');
    if (dotPos != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }

    if (text.empty() || text == "-0") {
        return "0";
    }
    return text;
}

} // namespace

NormalOrderService::NormalOrderService(IRestClient& rest, OrdersConfig cfg)
    : m_rest(rest),
      m_cfg(std::move(cfg)),
      m_validator(m_cfg),
      m_mapper(m_cfg),
      m_idGenerator(m_cfg.clientIdNamespace) {
    if (m_cfg.journal) {
        m_journal = m_cfg.journal;
    } else if (m_cfg.journalIsDurable) {
        m_journal = std::make_shared<DurableOrderJournal>(m_cfg.journalPath);
    } else if (m_cfg.allowBestEffortJournal) {
        m_journal = std::make_shared<InMemoryOrderJournal>();
    }
}

int64_t NormalOrderService::unixMsNow() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

OrderErrorCategory NormalOrderService::mapErrorCategory(const BinanceError& error) {
    return orders::detail::mapErrorCategory(error);
}

bool NormalOrderService::isAmbiguousPlacementError(const BinanceError& error) {
    return orders::detail::isAmbiguousPlacementError(error);
}

std::optional<int> NormalOrderService::optionalCode(const BinanceError& error) {
    if (error.code == 0) {
        return std::nullopt;
    }
    return error.code;
}

OrdersResult<ClientOrderId> NormalOrderService::resolveClientOrderId(const std::optional<ClientOrderId>& provided) {
    if (provided.has_value()) {
        auto valid = m_idGenerator.validateClientOrderId(*provided);
        if (!valid) {
            return std::unexpected(valid.error());
        }
        return *provided;
    }
    return m_idGenerator.generateClientOrderId();
}

std::string NormalOrderService::serializeRequestParams(const OrderRequest& request) {
    std::string params;
    appendParam(params, "symbol", request.symbol);
    appendParam(params, "side", sideToString(request.side));
    appendParam(params, "type", typeToString(request.type));
    appendParam(params, "positionSide", positionSideToString(request.positionSide));
    appendParam(params, "quantity", request.quantity);
    appendParam(params, "price", request.price.value_or(""));
    appendParam(params, "stopPrice", request.stopPrice.value_or(""));
    appendParam(params, "activationPrice", request.activationPrice.value_or(""));
    appendParam(params, "callbackRate", request.callbackRate.value_or(""));
    appendParam(params, "timeInForce", request.timeInForce ? tifToString(*request.timeInForce) : "");
    appendParam(params, "reduceOnly", request.reduceOnly ? boolToString(*request.reduceOnly) : "");
    appendParam(params, "closePosition", request.closePosition ? boolToString(*request.closePosition) : "");
    appendParam(params,
                "workingType",
                request.workingType
                    ? (*request.workingType == WorkingType::MarkPrice ? "MARK_PRICE" : "CONTRACT_PRICE")
                    : "");
    appendParam(params, "newClientOrderId", request.newClientOrderId.value_or(""));
    appendParam(params, "newOrderRespType", request.newOrderRespType.value_or(""));
    appendParam(params, "recvWindow", request.recvWindow ? std::to_string(*request.recvWindow) : "");
    for (const auto& [k, v] : request.extraParams) {
        if (k == "signature" || k == "timestamp") {
            continue;
        }
        appendParam(params, k, v);
    }
    return params;
}

std::expected<void, BinanceError> NormalOrderService::recordIntent(const PreparedPlacement& placement) {
    if (!m_journal) {
        return std::unexpected(BinanceError::fromApiResponse(-90006, "No journal configured"));
    }
    const bool durableConfigured = m_cfg.journalIsDurable
        || dynamic_cast<DurableOrderJournal*>(m_journal.get()) != nullptr;
    if (!m_cfg.allowBestEffortJournal && !durableConfigured) {
        return std::unexpected(BinanceError::fromApiResponse(
            -90009, "Durable journal is required when allowBestEffortJournal=false"));
    }

    JournalEntry entry;
    entry.correlationId = placement.result.correlationId;
    entry.symbol = placement.result.symbol;
    entry.clientOrderId = placement.request.newClientOrderId.value_or(placement.result.clientOrderId);
    entry.orderCategory = "normal";
    entry.side = placement.request.side;
    entry.type = placement.request.type;
    entry.positionSide = placement.request.positionSide;
    entry.quantity = placement.request.quantity;
    entry.price = placement.request.price.value_or("");
    entry.requestParams = serializeRequestParams(placement.request);
    entry.sendTimestampMs = unixMsNow();
    entry.state = PlacementState::UnknownPendingReconcile;
    entry.metadata = placement.metadata;
    return m_journal->recordIntent(std::move(entry));
}

std::expected<void, BinanceError> NormalOrderService::updateJournal(const CorrelationId& id,
                                                                    PlacementState state,
                                                                    std::optional<int64_t> orderId) {
    if (!m_journal) {
        return {};
    }
    return m_journal->updateState(id, state, orderId);
}

OrdersResult<NormalOrderService::PreparedPlacement> NormalOrderService::prepareMarket(MarketOrderDraft draft) {
    ClientOrderId clientOrderId;
    std::optional<std::string> clientOrderIdError;
    if (auto resolved = resolveClientOrderId(draft.clientOrderId); resolved) {
        clientOrderId = *resolved;
    } else {
        clientOrderIdError = resolved.error().message;
    }
    draft.clientOrderId = clientOrderId;

    auto report = m_validator.validateMarket(draft);
    if (clientOrderIdError.has_value()) {
        addValidationIssue(report, "client_order_id_generation_failed", *clientOrderIdError);
    }

    NormalPlacementResult result;
    result.symbol = draft.symbol;
    result.clientOrderId = clientOrderId;
    result.correlationId = m_idGenerator.generateCorrelationId();
    result.endpoint = "/fapi/v1/order";
    result.validation = report;
    if (report.hasErrors()) {
        result.state = PlacementState::Rejected;
        result.errorCategory = OrderErrorCategory::Validation;
        return PreparedPlacement{.result = std::move(result)};
    }
    result.state = PlacementState::UnknownPendingReconcile;

    PreparedPlacement prepared;
    prepared.request = m_mapper.toOrderRequest(draft, clientOrderId);
    prepared.result = std::move(result);
    prepared.metadata = draft.metadata;
    return prepared;
}

OrdersResult<NormalOrderService::PreparedPlacement> NormalOrderService::prepareLimit(LimitOrderDraft draft) {
    ClientOrderId clientOrderId;
    std::optional<std::string> clientOrderIdError;
    if (auto resolved = resolveClientOrderId(draft.clientOrderId); resolved) {
        clientOrderId = *resolved;
    } else {
        clientOrderIdError = resolved.error().message;
    }
    draft.clientOrderId = clientOrderId;

    auto report = m_validator.validateLimit(draft);
    if (clientOrderIdError.has_value()) {
        addValidationIssue(report, "client_order_id_generation_failed", *clientOrderIdError);
    }

    NormalPlacementResult result;
    result.symbol = draft.symbol;
    result.clientOrderId = clientOrderId;
    result.correlationId = m_idGenerator.generateCorrelationId();
    result.endpoint = "/fapi/v1/order";
    result.validation = report;
    if (report.hasErrors()) {
        result.state = PlacementState::Rejected;
        result.errorCategory = OrderErrorCategory::Validation;
        return PreparedPlacement{.result = std::move(result)};
    }
    result.state = PlacementState::UnknownPendingReconcile;

    PreparedPlacement prepared;
    prepared.request = m_mapper.toOrderRequest(draft, clientOrderId);
    prepared.result = std::move(result);
    prepared.metadata = draft.metadata;
    return prepared;
}

OrdersResult<NormalOrderService::PreparedPlacement> NormalOrderService::prepareCloseByMarket(CloseByMarketDraft draft) {
    ClientOrderId clientOrderId;
    std::optional<std::string> clientOrderIdError;
    if (auto resolved = resolveClientOrderId(draft.clientOrderId); resolved) {
        clientOrderId = *resolved;
    } else {
        clientOrderIdError = resolved.error().message;
    }
    draft.clientOrderId = clientOrderId;

    auto report = m_validator.validateCloseByMarket(draft);
    if (clientOrderIdError.has_value()) {
        addValidationIssue(report, "client_order_id_generation_failed", *clientOrderIdError);
    }

    NormalPlacementResult result;
    result.symbol = draft.symbol;
    result.clientOrderId = clientOrderId;
    result.correlationId = m_idGenerator.generateCorrelationId();
    result.endpoint = "/fapi/v1/order";
    result.validation = report;
    if (report.hasErrors()) {
        result.state = PlacementState::Rejected;
        result.errorCategory = OrderErrorCategory::Validation;
        return PreparedPlacement{.result = std::move(result)};
    }
    result.state = PlacementState::UnknownPendingReconcile;

    PreparedPlacement prepared;
    prepared.request = m_mapper.toOrderRequest(draft, clientOrderId);
    prepared.result = std::move(result);
    prepared.metadata = draft.metadata;
    return prepared;
}

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> NormalOrderService::market(MarketOrderDraft draft) {
    const auto startedAt = std::chrono::steady_clock::now();
    auto prepared = prepareMarket(std::move(draft));
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    auto result = std::move(prepared->result);
    if (result.state == PlacementState::Rejected) {
        logPlacement(result, startedAt);
        co_return result;
    }

    if (auto journalResult = recordIntent(*prepared); !journalResult) {
        result.state = PlacementState::Rejected;
        result.errorCategory = OrderErrorCategory::Journal;
        attachErrorDetails(result, journalResult.error());
        logPlacement(result, startedAt);
        co_return result;
    }

    auto placed = co_await m_rest.newOrder(std::move(prepared->request));
    if (!placed) {
        result.state = isAmbiguousPlacementError(placed.error())
            ? PlacementState::UnknownPendingReconcile
            : PlacementState::Rejected;
        result.errorCategory = mapErrorCategory(placed.error());
        attachErrorDetails(result, placed.error());
        (void)updateJournal(result.correlationId, result.state, std::nullopt);
        logPlacement(result, startedAt);
        co_return result;
    }

    result.state = PlacementState::Accepted;
    result.orderId = placed->orderId;
    result.orderStatus = placed->status;
    (void)updateJournal(result.correlationId, PlacementState::Accepted, placed->orderId);
    logPlacement(result, startedAt);
    co_return result;
}

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> NormalOrderService::limit(LimitOrderDraft draft) {
    const auto startedAt = std::chrono::steady_clock::now();
    auto prepared = prepareLimit(std::move(draft));
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    auto result = std::move(prepared->result);
    if (result.state == PlacementState::Rejected) {
        logPlacement(result, startedAt);
        co_return result;
    }

    if (auto journalResult = recordIntent(*prepared); !journalResult) {
        result.state = PlacementState::Rejected;
        result.errorCategory = OrderErrorCategory::Journal;
        attachErrorDetails(result, journalResult.error());
        logPlacement(result, startedAt);
        co_return result;
    }

    auto placed = co_await m_rest.newOrder(std::move(prepared->request));
    if (!placed) {
        result.state = isAmbiguousPlacementError(placed.error())
            ? PlacementState::UnknownPendingReconcile
            : PlacementState::Rejected;
        result.errorCategory = mapErrorCategory(placed.error());
        attachErrorDetails(result, placed.error());
        (void)updateJournal(result.correlationId, result.state, std::nullopt);
        logPlacement(result, startedAt);
        co_return result;
    }

    result.state = PlacementState::Accepted;
    result.orderId = placed->orderId;
    result.orderStatus = placed->status;
    (void)updateJournal(result.correlationId, PlacementState::Accepted, placed->orderId);
    logPlacement(result, startedAt);
    co_return result;
}

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> NormalOrderService::closeByMarket(CloseByMarketDraft draft) {
    const auto startedAt = std::chrono::steady_clock::now();
    auto prepared = prepareCloseByMarket(std::move(draft));
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    auto result = std::move(prepared->result);
    if (result.state == PlacementState::Rejected) {
        logPlacement(result, startedAt);
        co_return result;
    }

    if (auto journalResult = recordIntent(*prepared); !journalResult) {
        result.state = PlacementState::Rejected;
        result.errorCategory = OrderErrorCategory::Journal;
        attachErrorDetails(result, journalResult.error());
        logPlacement(result, startedAt);
        co_return result;
    }

    auto placed = co_await m_rest.newOrder(std::move(prepared->request));
    if (!placed) {
        result.state = isAmbiguousPlacementError(placed.error())
            ? PlacementState::UnknownPendingReconcile
            : PlacementState::Rejected;
        result.errorCategory = mapErrorCategory(placed.error());
        attachErrorDetails(result, placed.error());
        (void)updateJournal(result.correlationId, result.state, std::nullopt);
        logPlacement(result, startedAt);
        co_return result;
    }

    result.state = PlacementState::Accepted;
    result.orderId = placed->orderId;
    result.orderStatus = placed->status;
    (void)updateJournal(result.correlationId, PlacementState::Accepted, placed->orderId);
    logPlacement(result, startedAt);
    co_return result;
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> NormalOrderService::amendLimitOrder(AmendLimitOrderDraft draft) {
    auto request = m_mapper.toOrderRequest(draft);
    auto amended = co_await m_rest.modifyOrder(std::move(request));
    if (!amended) {
        co_return std::unexpected(amended.error());
    }
    co_return toSnapshot(*amended);
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> NormalOrderService::amendLimitOrderByOrderId(
    Symbol symbol,
    OrderSide side,
    int64_t orderId,
    Quantity quantity,
    Price price,
    std::optional<ResponseType> responseType,
    std::optional<int64_t> recvWindow) {
    co_return co_await amendLimitOrder(AmendLimitOrderDraft{
        .identity = NormalOrderIdentity{.symbol = std::move(symbol), .orderId = orderId},
        .side = side,
        .quantity = std::move(quantity),
        .price = std::move(price),
        .responseType = responseType,
        .recvWindow = recvWindow});
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> NormalOrderService::amendLimitOrderByClientOrderId(
    Symbol symbol,
    OrderSide side,
    ClientOrderId clientOrderId,
    Quantity quantity,
    Price price,
    std::optional<ResponseType> responseType,
    std::optional<int64_t> recvWindow) {
    co_return co_await amendLimitOrder(AmendLimitOrderDraft{
        .identity = NormalOrderIdentity{.symbol = std::move(symbol), .clientOrderId = std::move(clientOrderId)},
        .side = side,
        .quantity = std::move(quantity),
        .price = std::move(price),
        .responseType = responseType,
        .recvWindow = recvWindow});
}

NormalCancelResult NormalOrderService::toCancelResult(const Order& order) {
    NormalCancelResult out;
    out.symbol = order.symbol;
    out.orderId = order.orderId;
    out.clientOrderId = order.clientOrderId;
    out.orderStatus = order.status;
    out.side = order.side == OrderSide::Buy ? "BUY" : "SELL";
    out.type = typeToString(order.type);
    out.origQty = order.origQty;
    out.executedQty = order.executedQty;
    out.price = order.price;
    return out;
}

NormalOrderSnapshot NormalOrderService::toSnapshot(const Order& order) {
    NormalOrderSnapshot out;
    out.symbol = order.symbol;
    out.orderId = order.orderId;
    out.clientOrderId = order.clientOrderId;
    out.side = order.side;
    out.type = order.type;
    out.positionSide = order.positionSide;
    out.timeInForce = order.timeInForce;
    out.status = order.status;
    out.price = order.price;
    out.origQty = order.origQty;
    out.executedQty = order.executedQty;
    out.avgPrice = order.avgPrice;
    out.cumQuote = order.cumQuote;
    out.reduceOnly = order.reduceOnly;
    out.closePosition = order.closePosition;
    out.stopPrice = order.stopPrice;
    out.workingType = order.workingType;
    out.time = order.time;
    out.updateTime = order.updateTime;
    return out;
}

boost::asio::awaitable<OrdersResult<NormalCancelResult>> NormalOrderService::cancelNormalByOrderId(Symbol symbol, int64_t orderId) {
    auto canceled = co_await m_rest.cancelOrder(std::move(symbol), orderId);
    if (!canceled) {
        co_return std::unexpected(canceled.error());
    }
    co_return toCancelResult(*canceled);
}

boost::asio::awaitable<OrdersResult<NormalCancelResult>> NormalOrderService::cancelNormalByClientOrderId(
    Symbol symbol,
    ClientOrderId clientOrderId) {
    auto canceled = co_await m_rest.cancelOrderByClientOrderId(std::move(symbol), std::move(clientOrderId));
    if (!canceled) {
        co_return std::unexpected(canceled.error());
    }
    co_return toCancelResult(*canceled);
}

boost::asio::awaitable<OrdersResult<void>> NormalOrderService::cancelAllNormal(Symbol symbol) {
    co_return co_await m_rest.cancelAllOrders(std::move(symbol));
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> NormalOrderService::queryNormalByOrderId(Symbol symbol, int64_t orderId) {
    auto queried = co_await m_rest.queryOrder(std::move(symbol), orderId);
    if (!queried) {
        co_return std::unexpected(queried.error());
    }
    co_return toSnapshot(*queried);
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> NormalOrderService::queryNormalByClientOrderId(
    Symbol symbol,
    ClientOrderId clientOrderId) {
    auto queried = co_await m_rest.queryOrderByClientOrderId(std::move(symbol), std::move(clientOrderId));
    if (!queried) {
        co_return std::unexpected(queried.error());
    }
    co_return toSnapshot(*queried);
}

boost::asio::awaitable<OrdersResult<std::vector<NormalOrderSnapshot>>> NormalOrderService::openNormalOrders(
    std::optional<Symbol> symbol) {
    auto open = co_await m_rest.openOrders(std::move(symbol));
    if (!open) {
        co_return std::unexpected(open.error());
    }
    std::vector<NormalOrderSnapshot> result;
    result.reserve(open->size());
    for (const auto& order : *open) {
        result.push_back(toSnapshot(order));
    }
    co_return result;
}

boost::asio::awaitable<OrdersResult<std::vector<NormalOrderSnapshot>>> NormalOrderService::queryAllNormal(
    Symbol symbol,
    std::optional<int64_t> startTime,
    std::optional<int64_t> endTime,
    int limit) {
    if (startTime && endTime) {
        constexpr int64_t kSevenDaysMs = 7LL * 24LL * 60LL * 60LL * 1000LL;
        if (*endTime <= *startTime || (*endTime - *startTime) >= kSevenDaysMs) {
            co_return std::unexpected(BinanceError::fromApiResponse(
                -90007, "queryAllNormal requires start/end window < 7 days"));
        }
    }
    auto all = co_await m_rest.allOrders(std::move(symbol), startTime, endTime, limit);
    if (!all) {
        co_return std::unexpected(all.error());
    }
    std::vector<NormalOrderSnapshot> result;
    result.reserve(all->size());
    for (const auto& order : *all) {
        result.push_back(toSnapshot(order));
    }
    co_return result;
}

boost::asio::awaitable<OrdersResult<OrderFillSummary>> NormalOrderService::queryOrderFillSummary(Symbol symbol, int64_t orderId) {
    auto tradesResult = co_await m_rest.userTrades(symbol, orderId);
    if (!tradesResult) {
        co_return std::unexpected(tradesResult.error());
    }

    const auto& trades = *tradesResult;
    if (trades.empty()) {
        co_return OrderFillSummary{
            .symbol = symbol,
            .orderId = orderId,
            .completeness = FillSummaryCompleteness::Unavailable,
            .executedQty = "0",
        };
    }

    Decimal50 totalQty = 0;
    Decimal50 totalQuoteQty = 0;
    Decimal50 totalPnl = 0;
    Decimal50 totalCommission = 0;
    std::optional<std::string> commissionAsset;
    bool sawValidTrade = false;
    bool hasParseError = false;
    bool hasCommissionValue = false;
    bool hasPnlValue = false;
    int64_t firstTime = std::numeric_limits<int64_t>::max();
    int64_t lastTime = 0;

    for (const auto& t : trades) {
        Decimal50 qty;
        Decimal50 price;
        if (!tryParseDecimal(t.qty, qty) || !tryParseDecimal(t.price, price)) {
            hasParseError = true;
            continue;
        }
        sawValidTrade = true;
        totalQty += qty;
        totalQuoteQty += (qty * price);
        firstTime = std::min(firstTime, t.time);
        lastTime = std::max(lastTime, t.time);

        Decimal50 tradePnl;
        if (tryParseDecimal(t.realizedPnl, tradePnl)) {
            totalPnl += tradePnl;
            hasPnlValue = true;
        } else {
            hasParseError = true;
        }

        Decimal50 tradeCommission;
        if (tryParseDecimal(t.commission, tradeCommission)) {
            totalCommission += tradeCommission;
            hasCommissionValue = true;
        } else {
            hasParseError = true;
        }

        if (!t.commissionAsset.empty()) {
            if (!commissionAsset) {
                commissionAsset = t.commissionAsset;
            } else if (*commissionAsset != t.commissionAsset) {
                commissionAsset = std::nullopt;
                hasParseError = true;
            }
        }
    }

    if (!sawValidTrade) {
        co_return OrderFillSummary{
            .symbol = symbol,
            .orderId = orderId,
            .completeness = FillSummaryCompleteness::Unavailable,
            .executedQty = "0",
        };
    }

    OrderFillSummary summary;
    summary.symbol = symbol;
    summary.orderId = orderId;
    summary.completeness = hasParseError ? FillSummaryCompleteness::Partial : FillSummaryCompleteness::Complete;
    summary.executedQty = formatDecimal(totalQty);

    if (totalQty > Decimal50(0)) {
        summary.avgEntryPrice = formatDecimal(totalQuoteQty / totalQty);
        summary.avgExitPrice = summary.avgEntryPrice;
    }

    if (hasPnlValue) {
        summary.realizedPnl = formatDecimal(totalPnl);
    }
    if (hasCommissionValue) {
        summary.commission = formatDecimal(totalCommission);
    }
    summary.commissionAsset = commissionAsset;
    if (firstTime != std::numeric_limits<int64_t>::max()) {
        summary.firstTradeTime = firstTime;
        summary.lastTradeTime = lastTime;
    }

    co_return summary;
}

boost::asio::awaitable<OrdersResult<BatchPlacementResult>> NormalOrderService::batchNormal(std::vector<NormalOrderDraft> drafts) {
    const auto startedAt = std::chrono::steady_clock::now();
    BatchPlacementResult output;
    output.items.resize(drafts.size());

    const auto batchValidation = m_validator.validateBatch(drafts);
    if (batchValidation.hasErrors()) {
        for (size_t i = 0; i < drafts.size(); ++i) {
            NormalPlacementResult item;
            item.state = PlacementState::Rejected;
            item.errorCategory = OrderErrorCategory::Validation;
            item.endpoint = "/fapi/v1/batchOrders";
            item.validation = batchValidation;
            logPlacement(item, startedAt);
            output.items[i] = std::move(item);
        }
        co_return output;
    }

    std::vector<OrderRequest> requests;
    requests.reserve(drafts.size());
    std::vector<size_t> requestToResultIndex;
    requestToResultIndex.reserve(drafts.size());
    std::vector<PreparedPlacement> preparedItems;
    preparedItems.reserve(drafts.size());

    for (size_t i = 0; i < drafts.size(); ++i) {
        OrdersResult<PreparedPlacement> prepared = std::visit(
            [this](auto&& d) -> OrdersResult<PreparedPlacement> {
                using T = std::decay_t<decltype(d)>;
                if constexpr (std::is_same_v<T, MarketOrderDraft>) {
                    return prepareMarket(d);
                } else if constexpr (std::is_same_v<T, LimitOrderDraft>) {
                    return prepareLimit(d);
                } else {
                    return prepareCloseByMarket(d);
                }
            },
            drafts[i]);

        if (!prepared) {
            co_return std::unexpected(prepared.error());
        }
        prepared->result.endpoint = "/fapi/v1/batchOrders";

        if (prepared->result.state == PlacementState::Rejected) {
            logPlacement(prepared->result, startedAt);
            output.items[i] = std::move(prepared->result);
            continue;
        }

        if (auto journalResult = recordIntent(*prepared); !journalResult) {
            prepared->result.state = PlacementState::Rejected;
            prepared->result.errorCategory = OrderErrorCategory::Journal;
            attachErrorDetails(prepared->result, journalResult.error());
            logPlacement(prepared->result, startedAt);
            output.items[i] = std::move(prepared->result);
            continue;
        }

        requestToResultIndex.push_back(i);
        requests.push_back(prepared->request);
        preparedItems.push_back(std::move(*prepared));
    }

    if (requests.empty()) {
        co_return output;
    }

    auto batchResult = co_await m_rest.batchOrders(std::move(requests));
    if (!batchResult) {
        for (size_t idx = 0; idx < requestToResultIndex.size(); ++idx) {
            auto& item = preparedItems[idx].result;
            item.state = isAmbiguousPlacementError(batchResult.error())
                ? PlacementState::UnknownPendingReconcile
                : PlacementState::Rejected;
            item.errorCategory = mapErrorCategory(batchResult.error());
            attachErrorDetails(item, batchResult.error());
            (void)updateJournal(item.correlationId, item.state, std::nullopt);
            logPlacement(item, startedAt);
            output.items[requestToResultIndex[idx]] = item;
        }
        co_return output;
    }

    for (size_t idx = 0; idx < requestToResultIndex.size(); ++idx) {
        auto& item = preparedItems[idx].result;
        auto& out = output.items[requestToResultIndex[idx]];
        if (idx >= batchResult->results.size()) {
            item.state = PlacementState::UnknownPendingReconcile;
            item.errorCategory = OrderErrorCategory::Unknown;
            item.binanceMessage = "Missing batch result item";
            item.rawResponseBody = item.binanceMessage;
            (void)updateJournal(item.correlationId, item.state, std::nullopt);
            logPlacement(item, startedAt);
            out = item;
            continue;
        }

        const auto& resultItem = batchResult->results[idx];
        if (!resultItem) {
            item.state = isAmbiguousPlacementError(resultItem.error())
                ? PlacementState::UnknownPendingReconcile
                : PlacementState::Rejected;
            item.errorCategory = mapErrorCategory(resultItem.error());
            attachErrorDetails(item, resultItem.error());
            (void)updateJournal(item.correlationId, item.state, std::nullopt);
            logPlacement(item, startedAt);
            out = item;
            continue;
        }

        item.state = PlacementState::Accepted;
        item.orderId = resultItem->orderId;
        item.orderStatus = resultItem->status;
        (void)updateJournal(item.correlationId, PlacementState::Accepted, resultItem->orderId);
        logPlacement(item, startedAt);
        out = item;
    }

    co_return output;
}

boost::asio::awaitable<OrdersResult<OrderPoolSnapshot>> NormalOrderService::openNormalOrderSnapshot(
    std::optional<Symbol> symbol) {
    auto ordersResult = co_await openNormalOrders(symbol);
    if (!ordersResult) {
        co_return std::unexpected(ordersResult.error());
    }

    OrderPoolSnapshot snapshot;
    snapshot.kind = OrderPoolKind::Open;
    snapshot.capturedAt = std::chrono::system_clock::now();
    for (auto& order : *ordersResult) {
        snapshot.orders.push_back(enrichWithMetadata(std::move(order)));
    }
    co_return snapshot;
}

boost::asio::awaitable<OrdersResult<OrderPoolSnapshot>> NormalOrderService::queryAllNormalSnapshot(
    Symbol symbol,
    std::optional<int64_t> startTime,
    std::optional<int64_t> endTime,
    int limit) {
    auto ordersResult = co_await queryAllNormal(symbol, startTime, endTime, limit);
    if (!ordersResult) {
        co_return std::unexpected(ordersResult.error());
    }

    OrderPoolSnapshot snapshot;
    snapshot.kind = OrderPoolKind::History;
    snapshot.capturedAt = std::chrono::system_clock::now();
    for (auto& order : *ordersResult) {
        snapshot.orders.push_back(enrichWithMetadata(std::move(order)));
    }
    co_return snapshot;
}

OrderView NormalOrderService::enrichWithMetadata(NormalOrderSnapshot snapshot) {
    OrderView view;
    view.identity = NormalOrderIdentity{
        .symbol = snapshot.symbol,
        .orderId = snapshot.orderId,
        .clientOrderId = snapshot.clientOrderId,
    };
    view.normal = std::move(snapshot);

    if (m_journal) {
        if (auto entryResult = m_journal->findByClientOrderId(view.normal.clientOrderId); entryResult) {
            if (auto& entryOpt = entryResult.value(); entryOpt) {
                view.metadata = entryOpt->metadata;
            }
        }
    }

    return view;
}
