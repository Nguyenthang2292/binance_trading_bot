#include "scanner/kline_cache.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace scanner {

KlineCache::KlineCache(size_t bufferSize) : m_bufferSize(bufferSize > 0 ? bufferSize : 1) {}

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

std::vector<std::string> KlineCache::symbols() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> out;
    out.reserve(m_data.size());
    for (const auto& [symbol, _] : m_data) {
        out.push_back(symbol);
    }
    return out;
}

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
