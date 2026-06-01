#pragma once

#include "types/market.h"

#include <deque>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
     * Invoke `fn` with a read-locked, const reference to the bucket's klines for
     * `symbol`/`interval` without copying. Returns `true` if the bucket exists
     * (and `fn` was invoked); `false` otherwise.
     *
     * This is the zero-copy alternative to `snapshot()` for read-only consumers
     * on the hot path (per-cycle ATR/beta/trailing/signature computations) that
     * would otherwise deep-copy the entire bucket on every call.
     *
     * Thread-safety: holds a shared/read lock for the duration of `fn`. Keep
     * `fn` short and non-blocking; the referenced container is valid only for
     * the duration of the call (do not retain it), and `fn` must not call back
     * into the cache (would deadlock / recurse the shared lock).
     */
    template <class Fn>
    bool read(std::string_view symbol, std::string_view interval, Fn&& fn) const {
        std::shared_lock lock(m_mutex);
        const auto itSymbol = m_data.find(std::string(symbol));
        if (itSymbol == m_data.end()) {
            return false;
        }
        const auto itInterval = itSymbol->second.find(std::string(interval));
        if (itInterval == itSymbol->second.end()) {
            return false;
        }
        std::forward<Fn>(fn)(itInterval->second);
        return true;
    }

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
