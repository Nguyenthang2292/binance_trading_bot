#include "engine/order_cap_controller.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace engine {

namespace {

std::string fmt2(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

} // namespace

TotalNotionalGuard::TotalNotionalGuard(OrderCapConfig config)
    : m_config(std::move(config)) {}

OrderCapResult TotalNotionalGuard::check(
    double proposedNotional,
    const account::AccountSnapshot& snapshot,
    const PositionTracker& tracker) const {
    if (!m_config.enabled) {
        return {OrderCapDecision::Allow, "order cap disabled"};
    }

    if (m_config.maxTotalNotionalPct <= 0.0) {
        throw std::invalid_argument("order cap max_total_notional_pct must be > 0");
    }

    if (proposedNotional <= 0.0) {
        return {OrderCapDecision::Allow, "proposed notional not positive"};
    }

    const double totalMarginBalance = snapshot.account.totalMarginBalance;
    const double cap = totalMarginBalance * m_config.maxTotalNotionalPct;
    const double totalOpen = sumOpenNotional(snapshot, tracker);
    const double totalAfter = totalOpen + proposedNotional;

    OrderCapResult result;
    result.totalOpenNotional = totalOpen;
    result.proposedNotional = proposedNotional;
    result.cap = cap;
    result.totalMarginBalance = totalMarginBalance;

    if (totalAfter > cap) {
        result.decision = OrderCapDecision::Block;
        result.reason =
            "total notional " + fmt2(totalOpen) +
            " + proposed " + fmt2(proposedNotional) +
            " = " + fmt2(totalAfter) +
            " > cap " + fmt2(cap) +
            " (" + fmt2(m_config.maxTotalNotionalPct * 100.0) +
            "% x totalMarginBalance " + fmt2(totalMarginBalance) + ")";
        return result;
    }

    result.decision = OrderCapDecision::Allow;
    result.reason =
        "total notional " + fmt2(totalOpen) +
        " + proposed " + fmt2(proposedNotional) +
        " = " + fmt2(totalAfter) +
        " <= cap " + fmt2(cap);
    return result;
}

double TotalNotionalGuard::remotePositionNotional(const Position& pos) {
    if (std::abs(pos.positionAmt) <= 0.0) {
        return 0.0;
    }
    if (std::abs(pos.notional) > 0.0) {
        return std::abs(pos.notional);
    }
    if (std::abs(pos.markPrice) > 0.0) {
        return std::abs(pos.positionAmt * pos.markPrice);
    }
    if (std::abs(pos.entryPrice) > 0.0) {
        return std::abs(pos.positionAmt * pos.entryPrice);
    }
    return 0.0;
}

double TotalNotionalGuard::sumOpenNotional(
    const account::AccountSnapshot& snapshot,
    const PositionTracker& tracker) {
    std::unordered_map<std::string, double> notionalBySymbol;

    auto addRemotePositions = [&](const std::vector<Position>& positions) {
        for (const auto& pos : positions) {
            const double notional = remotePositionNotional(pos);
            if (notional <= 0.0 || pos.symbol.empty()) {
                continue;
            }
            auto& current = notionalBySymbol[pos.symbol];
            current = std::max(current, notional);
        }
    };

    addRemotePositions(snapshot.account.positions);
    if (snapshot.positions.has_value()) {
        addRemotePositions(*snapshot.positions);
    }

    for (const auto& tracked : tracker.all()) {
        if (tracked.symbol.empty()) {
            continue;
        }
        auto& current = notionalBySymbol[tracked.symbol];
        current = std::max(current, std::abs(tracked.quantity * tracked.entryPrice));
    }

    double total = 0.0;
    for (const auto& [_, notional] : notionalBySymbol) {
        total += notional;
    }
    return total;
}

} // namespace engine
