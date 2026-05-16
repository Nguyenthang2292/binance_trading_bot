#include "engine/work_queue.h"

#include <algorithm>

#include "logger.h"

namespace engine {

std::vector<WorkItem> WorkQueue::build(
    const std::vector<std::string>& symbols,
    const strategy::StrategyRegistry& registry) {
    std::vector<WorkItem> out;
    const auto strategies = registry.all();
    if (strategies.empty() || symbols.empty()) {
        return out;
    }

    auto sortedSymbols = symbols;
    std::sort(sortedSymbols.begin(), sortedSymbols.end());
    if (strategies.size() > sortedSymbols.size()) {
        const size_t unscheduled = strategies.size() - sortedSymbols.size();
        Logger::instance().log(
            LogLevel::Warning,
            "work queue strategy starvation this cycle unscheduled=" + std::to_string(unscheduled) +
                " symbols=" + std::to_string(sortedSymbols.size()) +
                " strategies=" + std::to_string(strategies.size()));
    }

    for (size_t symbolIndex = 0; symbolIndex < sortedSymbols.size(); ++symbolIndex) {
        const auto* strategy = strategies[symbolIndex % strategies.size()];
        const auto& cfg = strategy->config();
        for (const auto& interval : cfg.intervals) {
            out.push_back(WorkItem{
                .symbol = sortedSymbols[symbolIndex],
                .interval = interval,
                .strategy = strategy,
            });
        }
    }
    return out;
}

} // namespace engine

