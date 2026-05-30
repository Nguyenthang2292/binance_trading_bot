#include "orders/mql4_adapter.h"

namespace orders::mql4 {

namespace {

struct ProtectionShape {
    PositionSide positionSide{PositionSide::Both};
    OrderSide closeSide{OrderSide::Sell};
};

ProtectionShape protectionShapeFor(TradeOperation operation) {
    switch (operation) {
        case TradeOperation::Buy:
        case TradeOperation::BuyLimit:
        case TradeOperation::BuyStop:
            return ProtectionShape{.positionSide = PositionSide::Long, .closeSide = OrderSide::Sell};
        case TradeOperation::Sell:
        case TradeOperation::SellLimit:
        case TradeOperation::SellStop:
            return ProtectionShape{.positionSide = PositionSide::Short, .closeSide = OrderSide::Buy};
    }
    return ProtectionShape{};
}

ValidationIssue makeProtectionWarning(std::string code, std::string message) {
    return ValidationIssue{
        .severity = ValidationIssue::Severity::Warning,
        .code = std::move(code),
        .message = std::move(message),
    };
}

bool isMarketEntryOperation(TradeOperation operation) {
    return operation == TradeOperation::Buy || operation == TradeOperation::Sell;
}

boost::asio::awaitable<std::optional<Quantity>> confirmedProtectionQuantity(
    Orders& orders,
    const MappedOrderSendDraft& sourceDraft,
    const NormalPlacementResult& placement) {
    if (!isMarketEntryOperation(sourceDraft.operation)) {
        co_return sourceDraft.quantity;
    }
    if (!placement.orderId.has_value()) {
        co_return std::nullopt;
    }

    auto queried = co_await orders.queryNormalByOrderId(sourceDraft.symbol, *placement.orderId);
    if (!queried) {
        co_return std::nullopt;
    }

    auto parsedQty = DecimalString::parse(queried->executedQty);
    if (!parsedQty || parsedQty->toDouble() <= 0.0) {
        co_return std::nullopt;
    }
    co_return *parsedQty;
}

boost::asio::awaitable<void> attachProtectionIfPresent(
    Orders& orders,
    const MappedOrderSendDraft& sourceDraft,
    NormalPlacementResult& placement) {
    if (placement.state != PlacementState::Accepted) {
        co_return;
    }
    if (!sourceDraft.stopLoss && !sourceDraft.takeProfit) {
        co_return;
    }

    const auto protectionQty = co_await confirmedProtectionQuantity(orders, sourceDraft, placement);
    if (!protectionQty) {
        placement.validation.issues.push_back(makeProtectionWarning(
            "mql4_protection_qty_unconfirmed",
            "skipped SL/TP attach because executed quantity is not confirmed"));
        co_return;
    }

    const auto shape = protectionShapeFor(sourceDraft.operation);
    const auto closeQuantity = std::variant<Quantity, CloseEntirePosition>{*protectionQty};

    if (sourceDraft.stopLoss) {
        ProtectionOrderDraft stopLossDraft{
            .symbol = sourceDraft.symbol,
            .positionSide = shape.positionSide,
            .closeSide = shape.closeSide,
            .kind = ProtectionKind::StopLoss,
            .triggerPrice = *sourceDraft.stopLoss,
            .closeQuantity = closeQuantity,
            .metadata = sourceDraft.metadata,
        };

        auto stopLossResult = co_await orders.protection(std::move(stopLossDraft));
        if (!stopLossResult) {
            placement.validation.issues.push_back(makeProtectionWarning(
                "mql4_stop_loss_attach_failed",
                "failed to attach stopLoss protection: " + stopLossResult.error().message));
        } else if (stopLossResult->state != PlacementState::Accepted) {
            placement.validation.issues.push_back(makeProtectionWarning(
                "mql4_stop_loss_attach_rejected",
                "stopLoss protection was not accepted"));
        }
    }

    if (sourceDraft.takeProfit) {
        ProtectionOrderDraft takeProfitDraft{
            .symbol = sourceDraft.symbol,
            .positionSide = shape.positionSide,
            .closeSide = shape.closeSide,
            .kind = ProtectionKind::TakeProfit,
            .triggerPrice = *sourceDraft.takeProfit,
            .closeQuantity = closeQuantity,
            .metadata = sourceDraft.metadata,
        };

        auto takeProfitResult = co_await orders.protection(std::move(takeProfitDraft));
        if (!takeProfitResult) {
            placement.validation.issues.push_back(makeProtectionWarning(
                "mql4_take_profit_attach_failed",
                "failed to attach takeProfit protection: " + takeProfitResult.error().message));
        } else if (takeProfitResult->state != PlacementState::Accepted) {
            placement.validation.issues.push_back(makeProtectionWarning(
                "mql4_take_profit_attach_rejected",
                "takeProfit protection was not accepted"));
        }
    }
}

} // namespace

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> Mql4Adapter::orderSend(MappedOrderSendDraft draft) {
    OrdersResult<NormalPlacementResult> result = std::unexpected(BinanceError::fromApiResponse(-1, "Unknown operation"));
    switch (draft.operation) {
        case TradeOperation::Buy: {
            result = co_await m_orders.market(MarketOrderDraft{
                .symbol = draft.symbol,
                .side = OrderSide::Buy,
                .quantity = draft.quantity,
                .metadata = draft.metadata,
            });
            break;
        }
        case TradeOperation::Sell: {
            result = co_await m_orders.market(MarketOrderDraft{
                .symbol = draft.symbol,
                .side = OrderSide::Sell,
                .quantity = draft.quantity,
                .metadata = draft.metadata,
            });
            break;
        }
        case TradeOperation::BuyLimit:
            if (!draft.price) {
                co_return std::unexpected(BinanceError::fromApiResponse(-1, "Price required for BuyLimit"));
            }
            result = co_await m_orders.limit(LimitOrderDraft{
                .symbol = draft.symbol,
                .side = OrderSide::Buy,
                .quantity = draft.quantity,
                .price = *draft.price,
                .metadata = draft.metadata,
            });
            break;
        case TradeOperation::SellLimit:
            if (!draft.price) {
                co_return std::unexpected(BinanceError::fromApiResponse(-1, "Price required for SellLimit"));
            }
            result = co_await m_orders.limit(LimitOrderDraft{
                .symbol = draft.symbol,
                .side = OrderSide::Sell,
                .quantity = draft.quantity,
                .price = *draft.price,
                .metadata = draft.metadata,
            });
            break;
        case TradeOperation::BuyStop:
            if (!draft.price) {
                co_return std::unexpected(BinanceError::fromApiResponse(-1, "Price required for BuyStop"));
            }
            result = co_await m_orders.stopEntry(StopEntryDraft{
                .symbol = draft.symbol,
                .side = OrderSide::Buy,
                .quantity = draft.quantity,
                .triggerPrice = *draft.price,
                .limitPrice = draft.limitPrice,
                .metadata = draft.metadata,
            });
            break;
        case TradeOperation::SellStop:
            if (!draft.price) {
                co_return std::unexpected(BinanceError::fromApiResponse(-1, "Price required for SellStop"));
            }
            result = co_await m_orders.stopEntry(StopEntryDraft{
                .symbol = draft.symbol,
                .side = OrderSide::Sell,
                .quantity = draft.quantity,
                .triggerPrice = *draft.price,
                .limitPrice = draft.limitPrice,
                .metadata = draft.metadata,
            });
            break;
    }

    if (!result) {
        co_return result;
    }
    co_await attachProtectionIfPresent(m_orders, draft, *result);
    co_return result;
}

boost::asio::awaitable<OrdersResult<OrderPoolSnapshot>> Mql4Adapter::getOpenOrders(std::optional<Symbol> symbol) {
    co_return co_await m_orders.openNormalOrderSnapshot(std::move(symbol));
}

} // namespace orders::mql4
