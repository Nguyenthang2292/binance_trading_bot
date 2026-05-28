#include "scanner/kline_cache.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace scanner {

/**
 * KlineCache constructor.
 *
 * @param bufferSize Maximum number of Kline entries to retain per (symbol, interval)
 * bucket. Values less than 1 are normalized to 1.
 *
 * The cache stores recent Kline (candlestick) data per symbol/interval pair
 * and maintains at most `m_bufferSize` entries per bucket.
 */
KlineCache::KlineCache(size_t bufferSize) : m_bufferSize(bufferSize > 0 ? bufferSize : 1) {}

/**
 * Update the cache with a single `kline` for the given `symbol` and `interval`.
 *
 * Thread-safety: acquires an exclusive `std::unique_lock` on `m_mutex` while
 * modifying the internal data structures.
 *
 * Behavior:
 * - If the target bucket is empty, the kline is appended.
 * - If the incoming kline has the same `openTime` as the last entry, the last
 *   entry is replaced (in-place update of the most recent candle).
 * - If the incoming kline is older than the last entry (out-of-order), the
 *   function unlocks and delegates to `merge()` to insert the kline in order
 *   (preserves deduplication semantics by `openTime`).
 * - After insertion, the bucket is trimmed from the front to ensure it never
 *   stores more than `m_bufferSize` items (keeps the most recent entries).
 *
 * @param symbol Market symbol (e.g., "BTCUSDT").
 * @param interval Time interval string (e.g., "1m").
 * @param kline The Kline (candlestick) data to insert or merge.
 */
void KlineCache::update(std::string_view symbol, std::string_view interval, const Kline& kline) {
    std::unique_lock lock(m_mutex);
    auto& bucket = m_data[std::string(symbol)][std::string(interval)];
    if (!bucket.empty() && bucket.back().openTime == kline.openTime) {
        bucket.back() = kline;
        return;
    }
    if (!bucket.empty() && kline.openTime < bucket.back().openTime) {
        std::vector<Kline> one{kline};
        lock.unlock();
        merge(symbol, interval, one);
        return;
    }
    bucket.push_back(kline);
    while (bucket.size() > m_bufferSize) {
        bucket.pop_front();
    }
}

/**
 * Merge a sequence of `klines` into the cache for the specified symbol/interval.
 *
 * Thread-safety: acquires an exclusive `std::unique_lock` on `m_mutex` while
 * computing and replacing the target bucket.
 *
 * Behavior:
 * - Deduplicates entries by `openTime`: later entries override earlier ones for
 *   the same `openTime`.
 * - Combines existing bucket entries with the provided `klines`, then sorts
 *   by `openTime` to maintain chronological order.
 * - Keeps only the most recent `m_bufferSize` entries (drops older candles).
 * - If `klines` is empty, the call is a no-op.
 *
 * This is the safe path used when out-of-order updates must be merged into the
 * existing time-ordered sequence.
 *
 * @param symbol Market symbol to merge into.
 * @param interval Time interval string.
 * @param klines Span of Kline objects to merge.
 */
void KlineCache::merge(std::string_view symbol, std::string_view interval, std::span<const Kline> klines) {
    if (klines.empty()) {
        return;
    }

    std::unique_lock lock(m_mutex);
    auto& bucket = m_data[std::string(symbol)][std::string(interval)];

    std::unordered_map<int64_t, Kline> mergedByOpenTime;
    mergedByOpenTime.reserve(bucket.size() + klines.size());
    for (const auto& kline : bucket) {
        mergedByOpenTime[kline.openTime] = kline;
    }
    for (const auto& kline : klines) {
        mergedByOpenTime[kline.openTime] = kline;
    }

    std::vector<Kline> ordered;
    ordered.reserve(mergedByOpenTime.size());
    for (const auto& [_, kline] : mergedByOpenTime) {
        ordered.push_back(kline);
    }
    std::sort(ordered.begin(), ordered.end(), [](const Kline& a, const Kline& b) {
        return a.openTime < b.openTime;
    });

    const size_t keepFrom = ordered.size() > m_bufferSize ? (ordered.size() - m_bufferSize) : 0;
    bucket.clear();
    for (size_t i = keepFrom; i < ordered.size(); ++i) {
        bucket.push_back(ordered[i]);
    }
}

/**
 * Take a snapshot (copy) of the current bucket for `symbol` and `interval`.
 *
 * Thread-safety: acquires a shared/read `std::shared_lock` on `m_mutex` so
 * concurrent readers do not block each other. The returned vector is a copy and
 * safe for the caller to use without holding locks.
 *
 * @param symbol Market symbol to snapshot.
 * @param interval Time interval string.
 * @return `std::optional<std::vector<Kline>>` containing the bucket copy, or
 * `std::nullopt` if the symbol or interval is not present in the cache.
 */
std::optional<std::vector<Kline>> KlineCache::snapshot(std::string_view symbol, std::string_view interval) const {
    std::shared_lock lock(m_mutex);
    const auto itSymbol = m_data.find(std::string(symbol));
    if (itSymbol == m_data.end()) {
        return std::nullopt;
    }
    const auto itInterval = itSymbol->second.find(std::string(interval));
    if (itInterval == itSymbol->second.end()) {
        return std::nullopt;
    }
    return std::vector<Kline>(itInterval->second.begin(), itInterval->second.end());
}

/**
 * Return a list of symbols currently stored in the cache.
 *
 * Thread-safety: read-only operation that acquires a shared lock.
 *
 * @return Vector of symbol strings (copy of keys in the internal map).
 */
std::vector<std::string> KlineCache::symbols() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> out;
    out.reserve(m_data.size());
    for (const auto& [symbol, _] : m_data) {
        out.push_back(symbol);
    }
    return out;
}

/**
 * Return a list of unique intervals currently present in the cache across all
 * symbols.
 *
 * Thread-safety: read-only operation that acquires a shared lock. The result
 * contains unique interval strings; order is unspecified.
 *
 * @return Vector of interval strings.
 */
std::vector<std::string> KlineCache::intervals() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> out;
    for (const auto& [_, byInterval] : m_data) {
        for (const auto& [interval, __] : byInterval) {
            if (std::find(out.begin(), out.end(), interval) == out.end()) {
                out.push_back(interval);
            }
        }
    }
    return out;
}

} // namespace scanner
