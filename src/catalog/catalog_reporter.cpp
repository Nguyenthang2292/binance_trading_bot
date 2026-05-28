#include "catalog/catalog_reporter.h"

#include "logger.h"

#include <iostream>
#include <sstream>

namespace catalog {

namespace {

std::string joinIntervals(const std::vector<std::string>& intervals) {
    std::ostringstream out;
    for (size_t i = 0; i < intervals.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << intervals[i];
    }
    return out.str();
}

} // namespace

void CatalogReporter::printList(
    const std::vector<StrategyCatalog::StrategyInfo>& strategies,
    int pluginsLoaded,
    std::string_view pluginsDir) {
    std::cout << "Loaded " << strategies.size() << " strategies from " << pluginsLoaded
              << " plugins in " << pluginsDir << "/\n\n";
    int index = 1;
    for (const auto& strategy : strategies) {
        std::cout << "  [" << index++ << "] " << strategy.name << '\n'
                  << "      Type    : " << strategy.type << '\n'
                  << "      Version : " << strategy.version << '\n'
                  << "      Plugin  : " << strategy.pluginFile << '\n'
                  << "      Intervals: " << joinIntervals(strategy.intervals) << "\n\n";
    }
}

void CatalogReporter::logStartupSummary(
    const StrategyCatalog::LoadSummary& summary,
    const std::vector<StrategyCatalog::StrategyInfo>& strategies) {
    Logger::instance().log(
        LogLevel::Info,
        "catalog startup: plugins_found=" + std::to_string(summary.pluginsFound) +
            " plugins_loaded=" + std::to_string(summary.pluginsLoaded) +
            " strategies_registered=" + std::to_string(summary.strategiesRegistered));

    for (const auto& error : summary.errors) {
        Logger::instance().log(LogLevel::Warning, "catalog error: " + error);
    }

    for (const auto& strategy : strategies) {
        std::ostringstream out;
        out << "catalog strategy: name=" << strategy.name
            << " type=" << strategy.type
            << " version=" << strategy.version
            << " plugin=" << strategy.pluginFile;
        Logger::instance().log(LogLevel::Info, out.str());
    }
}

void CatalogReporter::logRuntimeStatus(
    const std::vector<StrategyCatalog::StrategyInfo>& strategies,
    int queueItems,
    int openPositions) {
    Logger::instance().log(
        LogLevel::Info,
        "strategy status: active=" + std::to_string(strategies.size()) +
            " queue=" + std::to_string(queueItems) +
            " open_positions=" + std::to_string(openPositions));

    for (const auto& strategy : strategies) {
        Logger::instance().log(
            LogLevel::Info,
            "strategy active: " + strategy.name +
                " intervals=[" + joinIntervals(strategy.intervals) + "]" +
                " scan=" + std::to_string(strategy.scanInterval.count()) + "s" +
                " hold=" + std::to_string(strategy.maxHoldDuration.count()) + "s");
    }
}

} // namespace catalog
