#include "orders/algo_order_service.h"
#include "orders/order_service_utils.h"

#include "logger.h"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace {
using orders::detail::addValidationIssue;
using orders::detail::attachErrorDetails;
using orders::detail::errorCategoryToString;
using orders::detail::extractTimeframe;
using orders::detail::isAmbiguousPlacementError;
using orders::detail::mapErrorCategory;
using orders::detail::stateToString;
using orders::detail::typeToString;

void logPlacement(
    const NormalPlacementResult& result,
    std::chrono::steady_clock::time_point startedAt,
    const std::optional<OrderMetadata>& metadata = std::nullopt) {
    const auto latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt).count();

    std::ostringstream out;
    out << "algo_order_placement"
        << " correlationId=" << result.correlationId
        << " clientOrderId=" << result.clientOrderId
        << " symbol=" << result.symbol
        << " endpoint=" << result.endpoint.value_or("")
        << " latencyMs=" << latencyMs
        << " state=" << stateToString(result.state)
        << " errorCategory=" << errorCategoryToString(result.errorCategory)
        << " binanceCode=" << result.binanceCode.value_or(0);
    if (result.binanceMessage.has_value()) {
        out << " binanceMessage=" << std::quoted(*result.binanceMessage);
    }
    if (metadata && metadata->strategyTag.has_value()) {
        out << " strategy=" << std::quoted(*metadata->strategyTag);
    }
    if (const auto timeframe = extractTimeframe(metadata); timeframe.has_value()) {
        out << " tf=" << *timeframe;
    }
    Logger::instance().log(LogLevel::Info, out.str());
}

NormalOrderSnapshot toSnapshot(const Order& order) {
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

NormalCancelResult toCancelResult(const Order& order) {
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

} // namespace

AlgoOrderService::AlgoOrderService(IRestClient& rest, OrdersConfig cfg)
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

int64_t AlgoOrderService::unixMsNow() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

OrderErrorCategory AlgoOrderService::mapErrorCategory(const BinanceError& error) {
    return orders::detail::mapErrorCategory(error);
}

bool AlgoOrderService::isAmbiguousPlacementError(const BinanceError& error) {
    return orders::detail::isAmbiguousPlacementError(error);
}

OrdersResult<ClientOrderId> AlgoOrderService::resolveClientOrderId(const std::optional<ClientOrderId>& provided) {
    if (provided.has_value()) {
        auto valid = m_idGenerator.validateClientOrderId(*provided);
        if (!valid) {
            return std::unexpected(valid.error());
        }
        return *provided;
    }
    return m_idGenerator.generateClientOrderId();
}

std::expected<void, BinanceError> AlgoOrderService::recordIntent(const PreparedPlacement& placement) {
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
    entry.orderCategory = "algo";
    entry.side = placement.request.side;
    entry.type = placement.request.type;
    entry.positionSide = placement.request.positionSide;
    entry.quantity = placement.request.quantity;
    entry.price = placement.request.price.value_or("");
    entry.sendTimestampMs = unixMsNow();
    entry.state = PlacementState::UnknownPendingReconcile;
    entry.metadata = placement.metadata;
    return m_journal->recordIntent(std::move(entry));
}

std::expected<void, BinanceError> AlgoOrderService::updateJournal(const CorrelationId& id,
                                                                  PlacementState state,
                                                                  std::optional<int64_t> orderId) {
    if (!m_journal) {
        return {};
    }
    return m_journal->updateState(id, state, orderId);
}

OrdersResult<AlgoOrderService::PreparedPlacement> AlgoOrderService::prepareStopEntry(StopEntryDraft draft) {
    ClientOrderId clientOrderId;
    std::optional<std::string> clientOrderIdError;
    if (auto resolved = resolveClientOrderId(draft.clientAlgoId); resolved) {
        clientOrderId = *resolved;
    } else {
        clientOrderIdError = resolved.error().message;
    }
    draft.clientAlgoId = clientOrderId;

    auto report = m_validator.validateStopEntry(draft);
    if (clientOrderIdError.has_value()) {
        addValidationIssue(report, "client_order_id_generation_failed", *clientOrderIdError);
    }

    NormalPlacementResult result;
    result.symbol = draft.symbol;
    result.clientOrderId = clientOrderId;
    result.correlationId = m_idGenerator.generateCorrelationId();
    result.endpoint = "/fapi/v1/algoOrder";
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

OrdersResult<AlgoOrderService::PreparedPlacement> AlgoOrderService::prepareProtection(ProtectionOrderDraft draft) {
    ClientOrderId clientOrderId;
    std::optional<std::string> clientOrderIdError;
    if (auto resolved = resolveClientOrderId(draft.clientAlgoId); resolved) {
        clientOrderId = *resolved;
    } else {
        clientOrderIdError = resolved.error().message;
    }
    draft.clientAlgoId = clientOrderId;

    auto report = m_validator.validateProtection(draft);
    if (clientOrderIdError.has_value()) {
        addValidationIssue(report, "client_order_id_generation_failed", *clientOrderIdError);
    }

    NormalPlacementResult result;
    result.symbol = draft.symbol;
    result.clientOrderId = clientOrderId;
    result.correlationId = m_idGenerator.generateCorrelationId();
    result.endpoint = "/fapi/v1/algoOrder";
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

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> AlgoOrderService::stopEntry(StopEntryDraft draft) {
    const auto startedAt = std::chrono::steady_clock::now();
    auto prepared = prepareStopEntry(std::move(draft));
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    auto result = std::move(prepared->result);
    if (result.state == PlacementState::Rejected) {
        logPlacement(result, startedAt, prepared->metadata);
        co_return result;
    }

    if (auto journalResult = recordIntent(*prepared); !journalResult) {
        result.state = PlacementState::Rejected;
        result.errorCategory = OrderErrorCategory::Journal;
        attachErrorDetails(result, journalResult.error());
        logPlacement(result, startedAt, prepared->metadata);
        co_return result;
    }

    auto placed = co_await m_rest.newAlgoOrder(std::move(prepared->request));
    if (!placed) {
        result.state = isAmbiguousPlacementError(placed.error())
            ? PlacementState::UnknownPendingReconcile
            : PlacementState::Rejected;
        result.errorCategory = mapErrorCategory(placed.error());
        attachErrorDetails(result, placed.error());
        (void)updateJournal(result.correlationId, result.state, std::nullopt);
        logPlacement(result, startedAt, prepared->metadata);
        co_return result;
    }

    result.state = PlacementState::Accepted;
    result.orderId = placed->orderId;
    result.orderStatus = placed->status;
    result.avgPrice = placed->avgPrice;
    (void)updateJournal(result.correlationId, PlacementState::Accepted, placed->orderId);
    logPlacement(result, startedAt, prepared->metadata);
    co_return result;
}

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> AlgoOrderService::protection(ProtectionOrderDraft draft) {
    const auto startedAt = std::chrono::steady_clock::now();
    auto prepared = prepareProtection(std::move(draft));
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    auto result = std::move(prepared->result);
    if (result.state == PlacementState::Rejected) {
        logPlacement(result, startedAt, prepared->metadata);
        co_return result;
    }

    if (auto journalResult = recordIntent(*prepared); !journalResult) {
        result.state = PlacementState::Rejected;
        result.errorCategory = OrderErrorCategory::Journal;
        attachErrorDetails(result, journalResult.error());
        logPlacement(result, startedAt, prepared->metadata);
        co_return result;
    }

    auto placed = co_await m_rest.newAlgoOrder(std::move(prepared->request));
    if (!placed) {
        result.state = isAmbiguousPlacementError(placed.error())
            ? PlacementState::UnknownPendingReconcile
            : PlacementState::Rejected;
        result.errorCategory = mapErrorCategory(placed.error());
        attachErrorDetails(result, placed.error());
        (void)updateJournal(result.correlationId, result.state, std::nullopt);
        logPlacement(result, startedAt, prepared->metadata);
        co_return result;
    }

    result.state = PlacementState::Accepted;
    result.orderId = placed->orderId;
    result.orderStatus = placed->status;
    result.avgPrice = placed->avgPrice;
    (void)updateJournal(result.correlationId, PlacementState::Accepted, placed->orderId);
    logPlacement(result, startedAt, prepared->metadata);
    co_return result;
}

boost::asio::awaitable<OrdersResult<NormalCancelResult>> AlgoOrderService::cancelAlgoByAlgoId(Symbol symbol, int64_t algoId) {
    auto canceled = co_await m_rest.cancelAlgoOrder(std::move(symbol), algoId);
    if (!canceled) {
        co_return std::unexpected(canceled.error());
    }
    co_return toCancelResult(*canceled);
}

boost::asio::awaitable<OrdersResult<NormalCancelResult>> AlgoOrderService::cancelAlgoByClientAlgoId(
    Symbol symbol,
    ClientAlgoId clientAlgoId) {
    auto canceled = co_await m_rest.cancelAlgoOrderByClientAlgoId(std::move(symbol), std::move(clientAlgoId));
    if (!canceled) {
        co_return std::unexpected(canceled.error());
    }
    co_return toCancelResult(*canceled);
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> AlgoOrderService::queryAlgoByAlgoId(Symbol symbol, int64_t algoId) {
    auto queried = co_await m_rest.queryAlgoOrder(std::move(symbol), algoId);
    if (!queried) {
        co_return std::unexpected(queried.error());
    }
    co_return toSnapshot(*queried);
}

boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> AlgoOrderService::queryAlgoByClientAlgoId(
    Symbol symbol,
    ClientAlgoId clientAlgoId) {
    auto queried = co_await m_rest.queryAlgoOrderByClientAlgoId(std::move(symbol), std::move(clientAlgoId));
    if (!queried) {
        co_return std::unexpected(queried.error());
    }
    co_return toSnapshot(*queried);
}
