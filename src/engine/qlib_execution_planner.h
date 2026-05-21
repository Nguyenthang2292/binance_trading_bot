#pragma once

#include "engine/iexecution_planner.h"
#include <string>

namespace engine {

class QlibExecutionPlanner : public IExecutionPlanner {
public:
    explicit QlibExecutionPlanner(const std::string& dbPath);
    QlibExecutionPlanner(const std::string& dbPath, IExecutionPlanner& nativeFallback);
    ~QlibExecutionPlanner() override;
    
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> executeMarket(MarketOrderDraft draft) override;

private:
    std::string m_dbPath;
    IExecutionPlanner* m_nativeFallback{nullptr};
};

} // namespace engine
