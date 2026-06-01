#pragma once

#include "common/expected_compat.h"
#include "orders/order_types.h"
#include "types/error.h"
#include "types/account.h"
#include "types/market.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

class BinanceContext;
class RestClient;
class RestClientAdapter;
class Orders;

class BinanceAPI {
public:
    BinanceAPI(const std::string& apiKey, const std::string& secretKey);
    ~BinanceAPI();

    // Legacy wrapper is restricted to durable journaling + RESULT response semantics.
    static OrdersConfig makeLegacyOrdersConfig();

    std::optional<double> getPrice(const std::string& symbol);
    std::vector<Kline> getKlines(const std::string& symbol, const std::string& interval, int limit = 100);
    std::vector<Ticker> get24hrTickers();
    std::optional<AccountInfo> getAccountInfo();
    std::optional<std::vector<Position>> getPositions(const std::string& symbol);
    bool testConnectivity();

    compat::expected<NormalPlacementResult, BinanceError> marketOrder(MarketOrderDraft draft);
    compat::expected<NormalPlacementResult, BinanceError> limitOrder(LimitOrderDraft draft);
    compat::expected<NormalPlacementResult, BinanceError> closeByMarket(CloseByMarketDraft draft);
    compat::expected<NormalCancelResult, BinanceError> cancelNormalByOrderId(const Symbol& symbol, int64_t orderId);
    compat::expected<NormalOrderSnapshot, BinanceError> queryNormalByOrderId(const Symbol& symbol, int64_t orderId);

private:
    std::unique_ptr<BinanceContext> m_context;
    std::unique_ptr<RestClient> m_rest;
    std::unique_ptr<RestClientAdapter> m_ordersAdapter;
    std::unique_ptr<Orders> m_orders;
};
