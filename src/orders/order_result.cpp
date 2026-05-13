#include "orders/order_result.h"
#include <algorithm>
#include <sstream>

std::optional<OrderView> OrderPoolSnapshot::atSnapshotIndex(size_t index) const {
    if (index >= orders.size()) {
        return std::nullopt;
    }
    return orders[index];
}

std::optional<OrderView> OrderPoolSnapshot::byIdentity(const OrderIdentity& identity) const {
    auto it = std::find_if(orders.begin(), orders.end(), [&](const OrderView& view) {
        return view.identity == identity;
    });
    if (it != orders.end()) {
        return *it;
    }
    return std::nullopt;
}

namespace {
std::string sideToString(OrderSide side) {
    return side == OrderSide::Buy ? "BUY" : "SELL";
}

std::string typeToString(OrderType type) {
    switch (type) {
        case OrderType::Limit: return "LIMIT";
        case OrderType::Market: return "MARKET";
        case OrderType::Stop: return "STOP";
        case OrderType::StopMarket: return "STOP_MARKET";
        case OrderType::TakeProfit: return "TAKE_PROFIT";
        case OrderType::TakeProfitMarket: return "TAKE_PROFIT_MARKET";
        case OrderType::TrailingStopMarket: return "TRAILING_STOP_MARKET";
    }
    return "MARKET";
}
} // namespace

std::string formatOrderView(const OrderView& view, bool includeMetadata) {
    std::ostringstream out;
    const auto& normal = view.normal;
    out << "OrderView[" << normal.symbol << " " << normal.orderId << "]: "
        << normal.status << " " << sideToString(normal.side) << " " << typeToString(normal.type) << " "
        << "qty=" << normal.origQty << " exec=" << normal.executedQty << " price=" << normal.price;

    if (view.metadata) {
        if (view.metadata->magic) {
            out << " magic=" << *view.metadata->magic;
        }
        if (includeMetadata) {
            if (view.metadata->comment) {
                out << " comment=\"" << *view.metadata->comment << "\"";
            }
            if (view.metadata->strategyTag) {
                out << " tag=\"" << *view.metadata->strategyTag << "\"";
            }
        } else {
            if (view.metadata->comment || view.metadata->strategyTag) {
                out << " metadata=[REDACTED]";
            }
        }
    }
    return out.str();
}
