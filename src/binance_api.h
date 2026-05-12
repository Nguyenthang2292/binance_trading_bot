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

    std::optional<double> getPrice(const std::string& symbol);
    std::vector<Kline> getKlines(const std::string& symbol, const std::string& interval, int limit = 100);
    std::vector<Ticker> get24hrTickers();
    std::optional<AccountInfo> getAccountInfo();
    bool testConnectivity();

    std::expected<NormalPlacementResult, BinanceError> marketOrder(MarketOrderDraft draft);
    std::expected<NormalPlacementResult, BinanceError> limitOrder(LimitOrderDraft draft);
    std::expected<NormalPlacementResult, BinanceError> closeByMarket(CloseByMarketDraft draft);
    std::expected<NormalCancelResult, BinanceError> cancelNormalByOrderId(const Symbol& symbol, int64_t orderId);
    std::expected<NormalOrderSnapshot, BinanceError> queryNormalByOrderId(const Symbol& symbol, int64_t orderId);

private:
    std::unique_ptr<BinanceContext> m_context;
    std::unique_ptr<RestClient> m_rest;
    std::unique_ptr<RestClientAdapter> m_ordersAdapter;
    std::unique_ptr<Orders> m_orders;
};
