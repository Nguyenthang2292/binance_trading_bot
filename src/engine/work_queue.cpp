#include "engine/work_queue.h"

#include <algorithm>
#include <random>

#include "logger.h"

namespace engine {

namespace {

struct WorkBlock {
    std::string symbol;
    std::shared_ptr<const strategy::IStrategy> strategy;
};

} // namespace

std::vector<WorkItem> WorkQueue::build(
    const std::vector<std::string>& symbols,
    const strategy::StrategyRegistry& registry,
    std::optional<uint64_t> seed) {
    std::vector<WorkItem> out;
    auto strategies = registry.allShared();
    if (strategies.empty() || symbols.empty()) {
        return out;
    }

    const uint64_t rngSeed = seed.has_value() ? *seed : std::random_device{}();
    std::mt19937_64 rng(rngSeed);

    std::vector<WorkBlock> blocks;
    blocks.reserve(strategies.size() * symbols.size());

    for (const auto& strategy : strategies) {
        if (!strategy) {
            continue;
        }
        const auto& cfg = strategy->config();
        if (cfg.intervals.empty()) {
            continue;
        }
        for (const auto& symbol : symbols) {
            blocks.push_back(WorkBlock{
                .symbol = symbol,
                .strategy = strategy,
            });
        }
    }

    // Intentional scheduling design: shuffle complete strategy-symbol blocks.
    // Risk/exposure gates are order-sensitive; randomizing both dimensions
    // prevents a fixed strategy from permanently receiving first access to the
    // cycle budget while preserving interval order inside each pair.
    std::shuffle(blocks.begin(), blocks.end(), rng);

    for (const auto& block : blocks) {
        const auto& cfg = block.strategy->config();
        for (const auto& interval : cfg.intervals) {
            out.push_back(WorkItem{
                .symbol = block.symbol,
                .interval = interval,
                .strategy = block.strategy.get(),
                .strategyOwner = block.strategy,
            });
        }
    }
    return out;
}

} // namespace engine

