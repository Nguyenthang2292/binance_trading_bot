#pragma once

#include "catalog/strategy_catalog.h"

#include <string_view>

namespace catalog {

class CatalogReporter {
public:
    static void printList(
        const std::vector<StrategyCatalog::StrategyInfo>& strategies,
        int pluginsLoaded = 0,
        std::string_view pluginsDir = "plugins");
    static void logStartupSummary(
        const StrategyCatalog::LoadSummary& summary,
        const std::vector<StrategyCatalog::StrategyInfo>& strategies);
    static void logRuntimeStatus(
        const std::vector<StrategyCatalog::StrategyInfo>& strategies,
        int queueItems,
        int openPositions);
};

} // namespace catalog
