#pragma once

#include "types/trade.h"

#include <simdjson.h>

#include <string>
#include <string_view>

namespace ws_parse {

inline std::string_view asString(simdjson::dom::element element) {
    std::string_view s;
    if (!element.get(s)) return s;
    return {};
}

inline std::string stringField(simdjson::dom::element object, std::string_view field, std::string fallback = {}) {
    simdjson::dom::element value;
    if (object[field].get(value)) return fallback;
    auto s = asString(value);
    return s.empty() ? fallback : std::string(s);
}

inline int64_t intField(simdjson::dom::element object, std::string_view field, int64_t fallback = 0) {
    int64_t out = fallback;
    (void)object[field].get(out);
    return out;
}

inline bool boolField(simdjson::dom::element object, std::string_view field, bool fallback = false) {
    bool out = fallback;
    (void)object[field].get(out);
    return out;
}

inline double toDouble(simdjson::dom::element value, double fallback = 0.0) {
    double out = fallback;
    if (!value.get(out)) return out;
    auto s = asString(value);
    if (!s.empty()) {
        try { return std::stod(std::string(s)); } catch (...) { return fallback; }
    }
    return fallback;
}

inline double doubleField(simdjson::dom::element object, std::string_view field, double fallback = 0.0) {
    simdjson::dom::element value;
    if (object[field].get(value)) return fallback;
    return toDouble(value, fallback);
}

inline OrderSide parseSide(std::string_view value) { return value == "SELL" ? OrderSide::Sell : OrderSide::Buy; }

inline OrderType parseOrderType(std::string_view value) {
    if (value == "LIMIT") return OrderType::Limit;
    if (value == "STOP") return OrderType::Stop;
    if (value == "STOP_MARKET") return OrderType::StopMarket;
    if (value == "TAKE_PROFIT") return OrderType::TakeProfit;
    if (value == "TAKE_PROFIT_MARKET") return OrderType::TakeProfitMarket;
    if (value == "TRAILING_STOP_MARKET") return OrderType::TrailingStopMarket;
    return OrderType::Market;
}

inline PositionSide parsePositionSide(std::string_view value) {
    if (value == "LONG") return PositionSide::Long;
    if (value == "SHORT") return PositionSide::Short;
    return PositionSide::Both;
}

inline TimeInForce parseTimeInForce(std::string_view value) {
    if (value == "IOC") return TimeInForce::IOC;
    if (value == "FOK") return TimeInForce::FOK;
    if (value == "GTX") return TimeInForce::GTX;
    return TimeInForce::GTC;
}

inline WorkingType parseWorkingType(std::string_view value) {
    return value == "MARK_PRICE" ? WorkingType::MarkPrice : WorkingType::ContractPrice;
}

} // namespace ws_parse
