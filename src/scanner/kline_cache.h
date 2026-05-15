#pragma once

#include "types/market.h"

#include <deque>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace scanner {

class KlineCache {
public:
    explicit KlineCache(size_t bufferSize = 200);

    void update(std::string_view symbol, std::string_view interval, const Kline& kline);
    std::optional<std::vector<Kline>> snapshot(std::string_view symbol, std::string_view interval) const;

    std::vector<std::string> symbols() const;
    std::vector<std::string> intervals() const;

private:
    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, std::unordered_map<std::string, std::deque<Kline>>> m_data;
    size_t m_bufferSize;
};

} // namespace scanner

