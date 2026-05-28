#pragma once

#include "types/market.h"

#include <deque>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace scanner {

class KlineCache {
public:
    /**
     * Construct a KlineCache.
     *
     * @param bufferSize Maximum number of Kline entries to retain per
     * (symbol, interval) bucket. Values less than 1 are normalized to 1.
     */
    explicit KlineCache(size_t bufferSize = 200);

    /**
     * Insert or update a single `kline` for the given symbol and interval.
     *
     * Thread-safety: acquires an exclusive lock while modifying internal data.
     * See implementation for exact behavior on out-of-order updates.
     */
    void update(std::string_view symbol, std::string_view interval, const Kline& kline);

    /**
     * Merge a span of `klines` into the cache for the specified symbol/interval.
     *
     * Thread-safety: acquires an exclusive lock. Deduplicates by `openTime`,
     * sorts by `openTime`, and retains only the most recent `m_bufferSize`
     * entries.
     */
    void merge(std::string_view symbol, std::string_view interval, std::span<const Kline> klines);

    /**
     * Return a copy of the bucket for `symbol` and `interval`.
     *
     * Thread-safety: acquires a shared/read lock. Returns `std::nullopt` if the
     * symbol or interval is not present.
     */
    std::optional<std::vector<Kline>> snapshot(std::string_view symbol, std::string_view interval) const;

    /**
     * Get a list of symbols currently stored in the cache.
     * Thread-safe read operation.
     */
    std::vector<std::string> symbols() const;

    /**
     * Get a list of unique intervals present in the cache across all symbols.
     * Thread-safe read operation.
     */
    std::vector<std::string> intervals() const;

private:
    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, std::unordered_map<std::string, std::deque<Kline>>> m_data;
    size_t m_bufferSize;
};

} // namespace scanner
