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

    if (strategies.size() > symbols.size()) {
        const size_t unscheduled = strategies.size() - symbols.size();
        Logger::instance().log(
            LogLevel::Warning,
            "work queue per-cycle strategy rotation active unscheduled_this_cycle=" + std::to_string(unscheduled) +
                " symbols=" + std::to_string(symbols.size()) +
                " strategies=" + std::to_string(strategies.size()) +
                " (probabilistically fair over time via shuffle)");
    }

    auto shuffledSymbols = symbols;
    std::shuffle(shuffledSymbols.begin(), shuffledSymbols.end(), rng);

    for (size_t symbolIndex = 0; symbolIndex < shuffledSymbols.size(); ++symbolIndex) {
        const auto* strategy = strategies[symbolIndex % strategies.size()];
        if (!strategy) {
            continue;
        }
        const auto& cfg = strategy->config();
        for (const auto& interval : cfg.intervals) {
            out.push_back(WorkItem{
                .symbol = shuffledSymbols[symbolIndex],
                .interval = interval,
                .strategy = strategy,
            });
        }
    }
    return out;
}

} // namespace engine

