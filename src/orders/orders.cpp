#include "orders/orders.h"

#include <chrono>
#include <type_traits>

namespace {

void addValidationIssue(ValidationReport& report, std::string code, std::string message) {
    report.issues.push_back(ValidationIssue{
        .severity = ValidationIssue::Severity::Error,
        .code = std::move(code),
        .message = std::move(message),
    });
}

} // namespace

Orders::Orders(IRestClient& rest, OrdersConfig cfg)
    : m_rest(rest),
      m_cfg(std::move(cfg)),
      m_validator(m_cfg),
      m_mapper(m_cfg),
      m_idGenerator(m_cfg.clientIdNamespace) {
    if (m_cfg.journal) {
        m_journal = m_cfg.journal;
    } else if (m_cfg.allowBestEffortJournal) {
        m_journal = std::make_shared<InMemoryOrderJournal>();
    }
}

int64_t Orders::unixMsNow() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

OrderErrorCategory Orders::mapErrorCategory(const BinanceError& error) {
    switch (error.category) {
        case ErrorCategory::Auth: return OrderErrorCategory::Auth;
        case ErrorCategory::RateLimit: return OrderErrorCategory::RateLimit;
        case ErrorCategory::Network: return OrderErrorCategory::Network;
        case ErrorCategory::Parse: return OrderErrorCategory::Parse;
        case ErrorCategory::Api: return OrderErrorCategory::ExchangeReject;
    }
    return OrderErrorCategory::Unknown;
}

bool Orders::isAmbiguousPlacementError(const BinanceError& error) {
    if (error.category == ErrorCategory::Network || error.category == ErrorCategory::Parse) {
        return true;
    }
    if (error.category == ErrorCategory::Api) {
        // Binance codes where the order commit status is unknown after send:
        // -1007: Timeout waiting for response from backend server
        // -1008: Server is busy; execution status unknown
        if (error.code == -1007 || error.code == -1008) {
            return true;
        }
        // Fallback for non-JSON 5xx bodies where fromHttp sets code = httpStatus
        if (error.code >= 500 && error.code < 600) {
            return true;
        }
    }
    return false;
}

std::optional<int> Orders::optionalCode(const BinanceError& error) {
    if (error.code == 0) {
        return std::nullopt;
    }
    return error.code;
}

OrdersResult<ClientOrderId> Orders::resolveClientOrderId(const std::optional<ClientOrderId>& provided) {
    if (provided.has_value()) {
        auto valid = m_idGenerator.validateClientOrderId(*provided);
        if (!valid) {
            return std::unexpected(valid.error());
        }
        return *provided;
    }
    return m_idGenerator.generateClientOrderId();
}

std::expected<void, BinanceError> Orders::recordIntent(const PreparedPlacement& placement) {
    if (!m_journal) {
        return std::unexpected(BinanceError::fromApiResponse(-90006, "No journal configured"));
    }

    JournalEntry entry;
    entry.correlationId = placement.result.correlationId;
    entry.symbol = placement.result.symbol;
    entry.clientOrderId = placement.result.clientOrderId;
    entry.orderCategory = "normal";
    entry.side = placement.request.side;
    entry.type = placement.request.type;
    entry.positionSide = placement.request.positionSide;
    entry.quantity = placement.request.quantity;
    entry.price = placement.request.price.value_or("");
    entry.requestParams = "";
    entry.sendTimestampMs = unixMsNow();
    entry.state = PlacementState::UnknownPendingReconcile;
    return m_journal->recordIntent(std::move(entry));
}

std::expected<void, BinanceError> Orders::updateJournal(const CorrelationId& id,
                                                        PlacementState state,
                                                        std::optional<int64_t> orderId) {
    if (!m_journal) {
        return {};
    }
    return m_journal->updateState(id, state, orderId);
}

OrdersResult<Orders::PreparedPlacement> Orders::prepareMarket(MarketOrderDraft draft) {
    auto report = m_validator.validateMarket(draft);
    ClientOrderId clientOrderId;
    if (auto resolved = resolveClientOrderId(draft.clientOrderId); resolved) {
        clientOrderId = *resolved;
    } else {
        addValidationIssue(report, "client_order_id_generation_failed", resolved.error().message);
    }

    NormalPlacementResult result;
    result.symbol = draft.symbol;
    result.clientOrderId = clientOrderId;
    result.correlationId = m_idGenerator.generateCorrelationId();
    result.validation = report;
    if (report.hasErrors()) {
        result.state = PlacementState::Rejected;
        result.errorCategory = OrderErrorCategory::Validation;
        return PreparedPlacement{.result = std::move(result)};
    }

    PreparedPlacement prepared;
    prepared.request = m_mapper.toOrderRequest(draft, clientOrderId);
    prepared.result = std::move(result);
    return prepared;
}

OrdersResult<Orders::PreparedPlacement> Orders::prepareLimit(LimitOrderDraft draft) {
    auto report = m_validator.validateLimit(draft);
    ClientOrderId clientOrderId;
    if (auto resolved = resolveClientOrderId(draft.clientOrderId); resolved) {
        clientOrderId = *resolved;
    } else {
        addValidationIssue(report, "client_order_id_generation_failed", resolved.error().message);
    }

    NormalPlacementResult result;
    result.symbol = draft.symbol;
    result.clientOrderId = clientOrderId;
    result.correlationId = m_idGenerator.generateCorrelationId();
    result.validation = report;
    if (report.hasErrors()) {
        result.state = PlacementState::Rejected;
        result.errorCategory = OrderErrorCategory::Validation;
        return PreparedPlacement{.result = std::move(result)};
    }

    PreparedPlacement prepared;
    prepared.request = m_mapper.toOrderRequest(draft, clientOrderId);
    prepared.result = std::move(result);
    return prepared;
}

OrdersResult<Orders::PreparedPlacement> Orders::prepareCloseByMarket(CloseByMarketDraft draft) {
    auto report = m_validator.validateCloseByMarket(draft);
    ClientOrderId clientOrderId;
    if (auto resolved = resolveClientOrderId(draft.clientOrderId); resolved) {
        clientOrderId = *resolved;
    } else {
        addValidationIssue(report, "client_order_id_generation_failed", resolved.error().message);
    }

    NormalPlacementResult result;
    result.symbol = draft.symbol;
    result.clientOrderId = clientOrderId;
    result.correlationId = m_idGenerator.generateCorrelationId();
    result.validation = report;
    if (report.hasErrors()) {
        result.state = PlacementState::Rejected;
        result.errorCategory = OrderErrorCategory::Validation;
        return PreparedPlacement{.result = std::move(result)};
    }

    PreparedPlacement prepared;
    prepared.request = m_mapper.toOrderRequest(draft, clientOrderId);
    prepared.result = std::move(result);
    return prepared;
}

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> Orders::market(MarketOrderDraft draft) {
    auto prepared = prepareMarket(std::move(draft));
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    auto result = std::move(prepared->result);
    if (result.state == PlacementState::Rejected) {
        co_return result;
    }

    if (auto journalResult = recordIntent(*prepared); !journalResult) {
        result.state = PlacementState::Rejected;
        result.errorCategory = OrderErrorCategory::Journal;
        result.binanceCode = optionalCode(journalResult.error());
        result.binanceMessage = journalResult.error().message;
        co_return result;
    }

    auto placed = co_await m_rest.newOrder(std::move(prepared->request));
    if (!placed) {
        result.state = isAmbiguousPlacementError(placed.error())
            ? PlacementState::UnknownPendingReconcile
            : PlacementState::Rejected;
        result.errorCategory = mapErrorCategory(placed.error());
        result.binanceCode = optionalCode(placed.error());
        result.binanceMessage = placed.error().message;
        (void)updateJournal(result.correlationId, result.state, std::nullopt);
        co_return result;
    }

    result.state = PlacementState::Accepted;
    result.orderId = placed->orderId;
    result.orderStatus = placed->status;
    (void)updateJournal(result.correlationId, PlacementState::Accepted, placed->orderId);
    co_return result;
}

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> Orders::limit(LimitOrderDraft draft) {
    auto prepared = prepareLimit(std::move(draft));
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    auto result = std::move(prepared->result);
    if (result.state == PlacementState::Rejected) {
        co_return result;
    }

    if (auto journalResult = recordIntent(*prepared); !journalResult) {
        result.state = PlacementState::Rejected;
        result.errorCategory = OrderErrorCategory::Journal;
        result.binanceCode = optionalCode(journalResult.error());
        result.binanceMessage = journalResult.error().message;
        co_return result;
    }

    auto placed = co_await m_rest.newOrder(std::move(prepared->request));
    if (!placed) {
        result.state = isAmbiguousPlacementError(placed.error())
            ? PlacementState::UnknownPendingReconcile
            : PlacementState::Rejected;
        result.errorCategory = mapErrorCategory(placed.error());
        result.binanceCode = optionalCode(placed.error());
        result.binanceMessage = placed.error().message;
        (void)updateJournal(result.correlationId, result.state, std::nullopt);
        co_return result;
    }

    result.state = PlacementState::Accepted;
    result.orderId = placed->orderId;
    result.orderStatus = placed->status;
    (void)updateJournal(result.correlationId, PlacementState::Accepted, placed->orderId);
    co_return result;
}

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> Orders::closeByMarket(CloseByMarketDraft draft) {
    auto prepared = prepareCloseByMarket(std::move(draft));
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    auto result = std::move(prepared->result);
    if (result.state == PlacementState::Rejected) {
        co_return result;
    }

    if (auto journalResult = recordIntent(*prepared); !journalResult) {
        result.state = PlacementState::Rejected;
        result.errorCategory = OrderErrorCategory::Journal;
        result.binanceCode = optionalCode(journalResult.error());
        result.binanceMessage = journalResult.error().message;
        co_return result;
    }

    auto placed = co_await m_rest.newOrder(std::move(prepared->request));
    if (!placed) {
        result.state = isAmbiguousPlacementError(placed.error())
            ? PlacementState::UnknownPendingReconcile
            : PlacementState::Rejected;
        result.errorCategory = mapErrorCategory(placed.error());
        result.binanceCode = optionalCode(placed.error());
        result.binanceMessage = placed.error().message;
        (void)updateJournal(result.correlationId, result.state, std::nullopt);
        co_return result;
    }

    result.state = PlacementState::Accepted;
    result.orderId = placed->orderId;
    result.orderStatus = placed->status;
    (void)updateJournal(result.correlationId, PlacementState::Accepted, placed->orderId);
    co_return result;
}

NormalCancelResult Orders::toCancelResult(const Order& order) {
    NormalCancelResult out;
    out.symbol = order.symbol;
    out.orderId = order.orderId;
    out.clientOrderId = order.clientOrderId;
    out.orderStatus = order.status;
    out.side = order.side == OrderSide::Buy ? "BUY" : "SELL";
    switch (order.type) {
        case OrderType::Limit: out.type = "LIMIT"; break;
        case OrderType::Market: out.type = "MARKET"; break;
        case OrderType::Stop: out.type = "STOP"; break;
        case OrderType::StopMarket: out.type = "STOP_MARKET"; break;
        case OrderType::TakeProfit: out.type = "TAKE_PROFIT"; break;
        case OrderType::TakeProfitMarket: out.type = "TAKE_PROFIT_MARKET"; break;
        case OrderType::TrailingStopMarket: out.type = "TRAILING_STOP_MARKET"; break;
    }
    out.origQty = order.origQty;
    out.executedQty = order.executedQty;
    out.price = order.price;
    return out;
}

NormalOrderSnapshot Orders::toSnapshot(const Order& order) {
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

boost::asio::awaitable<OrdersResult<NormalCancelResult>> Orders::cancelNormalByOrderId(Symbol symbol, int64_t orderId) {
    auto canceled = co_await m_rest.cancelOrder(std::move(symbol), orderId);
    if (!canceled) {
        co_return std::unexpected(canceled.error());
    }
    co_return toCancelResult(*canceled);
}

boost::asio::awaitable<OrdersResult<NormalCancelResult>> Orders::cancelNormalByClientOrderId(
    Symbol symbol,
    ClientOrderId clientOrderId) {
    auto canceled = co_await m_rest.cancelOrderByClientOrderId(std::move(symbol), std::move(clientOrderId));
    if (!canceled) {
        co_return std::unexpected(canceled.error());
    }
    co_return toCancelResult(*canceled);
}

boost::asio::awaitable<OrdersResult<void>> Orders::cancelAllNormal(Symbol symbol) {
    co_return co_await m_rest.cancelAllOrders(std::move(symbol));
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> Orders::queryNormalByOrderId(Symbol symbol, int64_t orderId) {
    auto queried = co_await m_rest.queryOrder(std::move(symbol), orderId);
    if (!queried) {
        co_return std::unexpected(queried.error());
    }
    co_return toSnapshot(*queried);
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> Orders::queryNormalByClientOrderId(
    Symbol symbol,
    ClientOrderId clientOrderId) {
    auto queried = co_await m_rest.queryOrderByClientOrderId(std::move(symbol), std::move(clientOrderId));
    if (!queried) {
        co_return std::unexpected(queried.error());
    }
    co_return toSnapshot(*queried);
}

boost::asio::awaitable<OrdersResult<std::vector<NormalOrderSnapshot>>> Orders::openNormalOrders(std::optional<Symbol> symbol) {
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

boost::asio::awaitable<OrdersResult<std::vector<NormalOrderSnapshot>>> Orders::queryAllNormal(
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

boost::asio::awaitable<OrdersResult<BatchPlacementResult>> Orders::batchNormal(std::vector<NormalOrderDraft> drafts) {
    BatchPlacementResult output;
    output.items.resize(drafts.size());

    const auto batchValidation = m_validator.validateBatch(drafts);
    if (batchValidation.hasErrors()) {
        for (size_t i = 0; i < drafts.size(); ++i) {
            NormalPlacementResult item;
            item.state = PlacementState::Rejected;
            item.errorCategory = OrderErrorCategory::Validation;
            item.validation = batchValidation;
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

        if (prepared->result.state == PlacementState::Rejected) {
            output.items[i] = std::move(prepared->result);
            continue;
        }

        if (auto journalResult = recordIntent(*prepared); !journalResult) {
            prepared->result.state = PlacementState::Rejected;
            prepared->result.errorCategory = OrderErrorCategory::Journal;
            prepared->result.binanceCode = optionalCode(journalResult.error());
            prepared->result.binanceMessage = journalResult.error().message;
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
            item.binanceCode = optionalCode(batchResult.error());
            item.binanceMessage = batchResult.error().message;
            (void)updateJournal(item.correlationId, item.state, std::nullopt);
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
            (void)updateJournal(item.correlationId, item.state, std::nullopt);
            out = item;
            continue;
        }

        const auto& resultItem = batchResult->results[idx];
        if (!resultItem) {
            item.state = isAmbiguousPlacementError(resultItem.error())
                ? PlacementState::UnknownPendingReconcile
                : PlacementState::Rejected;
            item.errorCategory = mapErrorCategory(resultItem.error());
            item.binanceCode = optionalCode(resultItem.error());
            item.binanceMessage = resultItem.error().message;
            (void)updateJournal(item.correlationId, item.state, std::nullopt);
            out = item;
            continue;
        }

        item.state = PlacementState::Accepted;
        item.orderId = resultItem->orderId;
        item.orderStatus = resultItem->status;
        (void)updateJournal(item.correlationId, PlacementState::Accepted, resultItem->orderId);
        out = item;
    }

    co_return output;
}
