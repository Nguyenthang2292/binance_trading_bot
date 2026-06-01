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

class AlgoOrderService {
public:
    AlgoOrderService(IRestClient& rest, OrdersConfig cfg);

    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> stopEntry(StopEntryDraft draft);
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> protection(ProtectionOrderDraft draft);

    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelAlgoByAlgoId(Symbol symbol, int64_t algoId);
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelAlgoByClientAlgoId(
        Symbol symbol, ClientAlgoId clientAlgoId);

    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> queryAlgoByAlgoId(Symbol symbol, int64_t algoId);
    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> queryAlgoByClientAlgoId(
        Symbol symbol, ClientAlgoId clientAlgoId);

private:
    struct PreparedPlacement {
        OrderRequest request;
        NormalPlacementResult result;
        std::optional<OrderMetadata> metadata;
    };

    IRestClient& m_rest;
    OrdersConfig m_cfg;
    OrderValidator m_validator;
    OrderMapper m_mapper;
    OrderIdGenerator m_idGenerator;
    std::shared_ptr<OrderJournal> m_journal;

    OrdersResult<ClientOrderId> resolveClientOrderId(const std::optional<ClientOrderId>& provided);
    OrdersResult<PreparedPlacement> prepareStopEntry(StopEntryDraft draft);
    OrdersResult<PreparedPlacement> prepareProtection(ProtectionOrderDraft draft);
    compat::expected<void, BinanceError> recordIntent(const PreparedPlacement& placement);
    compat::expected<void, BinanceError> updateJournal(const CorrelationId& id,
                                                    PlacementState state,
                                                    std::optional<int64_t> orderId = std::nullopt);
    static OrderErrorCategory mapErrorCategory(const BinanceError& error);
    static bool isAmbiguousPlacementError(const BinanceError& error);
    static int64_t unixMsNow();
};
