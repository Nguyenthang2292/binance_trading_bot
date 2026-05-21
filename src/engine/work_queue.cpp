#include "engine/work_queue.h"

#include <algorithm>
#include <random>

#include "logger.h"

namespace engine {

std::vector<WorkItem> WorkQueue::build(
    const std::vector<std::string>& symbols,
    const strategy::StrategyRegistry& registry,
    std::optional<uint64_t> seed) {
    std::vector<WorkItem> out;
    auto strategies = registry.all();
    if (strategies.empty() || symbols.empty()) {
        return out;
    }

    const uint64_t rngSeed = seed.has_value() ? *seed : std::random_device{}();
    std::mt19937_64 rng(rngSeed);
    std::shuffle(strategies.begin(), strategies.end(), rng);

    auto shuffledSymbols = symbols;
    std::shuffle(shuffledSymbols.begin(), shuffledSymbols.end(), rng);

    for (const auto* strategy : strategies) {
        if (!strategy) {
            continue;
        }
        const auto& cfg = strategy->config();
        for (const auto& symbol : shuffledSymbols) {
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

