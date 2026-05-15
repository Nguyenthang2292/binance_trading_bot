#include "scanner/kline_cache.h"

#include <algorithm>
#include <mutex>

namespace scanner {

KlineCache::KlineCache(size_t bufferSize) : m_bufferSize(bufferSize > 0 ? bufferSize : 1) {}

void KlineCache::update(std::string_view symbol, std::string_view interval, const Kline& kline) {
    std::unique_lock lock(m_mutex);
    auto& bucket = m_data[std::string(symbol)][std::string(interval)];
    if (!bucket.empty() && bucket.back().openTime == kline.openTime) {
        bucket.back() = kline;
        return;
    }
    bucket.push_back(kline);
    while (bucket.size() > m_bufferSize) {
        bucket.pop_front();
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
