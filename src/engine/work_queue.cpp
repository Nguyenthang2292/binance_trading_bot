#include "engine/work_queue.h"

namespace engine {

std::vector<WorkItem> WorkQueue::build(
    const std::vector<std::string>& symbols,
    const strategy::StrategyRegistry& registry) {
    std::vector<WorkItem> out;
    const auto strategies = registry.all();
    for (const auto* strategy : strategies) {
        if (!strategy) {
            continue;
        }
        const auto& cfg = strategy->config();
        for (const auto& symbol : symbols) {
            for (const auto& interval : cfg.intervals) {
                out.push_back(WorkItem{
                    .symbol = symbol,
                    .interval = interval,
                    .strategy = strategy,
                });
            }
        }
    }
    return out;
}

} // namespace engine

