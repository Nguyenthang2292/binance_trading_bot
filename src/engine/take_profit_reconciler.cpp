#include "engine/take_profit_reconciler.h"

#include "engine/position_tracker.h"
#include "engine/price_filter.h"
#include "engine/signal_engine.h"
#include "logger.h"
#include "orders/decimal_string.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine {
namespace {

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string quoteString(std::string_view value) {
    std::ostringstream out;
    out << std::quoted(std::string(value));
    return out.str();
}

strategy::Signal::Direction directionFromPosition(double positionAmt) {
    if (positionAmt > 0.0) {
        return strategy::Signal::Direction::Long;
    }
    if (positionAmt < 0.0) {
        return strategy::Signal::Direction::Short;
    }
    return strategy::Signal::Direction::None;
}

OrderSide closeSide(strategy::Signal::Direction direction) {
    return direction == strategy::Signal::Direction::Long ? OrderSide::Sell : OrderSide::Buy;
}

std::vector<const Position*> livePositionsFromSnapshot(const account::AccountSnapshot& snapshot) {
    std::vector<const Position*> live;
    auto collect = [&live](const std::vector<Position>& positions) {
        for (const auto& position : positions) {
            if (!engine::isFlatPositionQuantity(position.positionAmt)) {
                live.push_back(&position);
            }
        }
    };

    if (snapshot.positions.has_value()) {
        collect(*snapshot.positions);
    } else {
        collect(snapshot.account.positions);
    }
    return live;
}

std::optional<double> parseDecimalValue(std::string_view raw) {
    auto parsed = DecimalString::parse(raw);
    if (!parsed) {
        return std::nullopt;
    }
    const double value = parsed->toDouble();
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

double remainingQuantity(const NormalOrderSnapshot& order) {
    const auto origQty = parseDecimalValue(order.origQty);
    const auto executedQty = parseDecimalValue(order.executedQty);
    if (!origQty.has_value() || !executedQty.has_value()) {
        return 0.0;
    }
    return std::max(0.0, *origQty - *executedQty);
}

bool isLiveOrderStatus(std::string_view status) {
    return status == "NEW" || status == "PARTIALLY_FILLED";
}

std::string toBase36(std::uint64_t value) {
    constexpr std::array<char, 36> kDigits{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
        'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
        'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't',
        'u', 'v', 'w', 'x', 'y', 'z'};
    if (value == 0) {
        return "0";
    }

    std::string out;
    while (value > 0) {
        out.push_back(kDigits[value % 36]);
        value /= 36;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::optional<std::uint64_t> fromBase36(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::uint64_t out = 0;
    for (char c : value) {
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'z') {
            digit = static_cast<unsigned>(10 + (c - 'a'));
        } else if (c >= 'A' && c <= 'Z') {
            digit = static_cast<unsigned>(10 + (c - 'A'));
        } else {
            return std::nullopt;
        }
        if (digit >= 36) {
            return std::nullopt;
        }
        if (out > (std::numeric_limits<std::uint64_t>::max() - digit) / 36ULL) {
            return std::nullopt;
        }
        out = out * 36ULL + static_cast<std::uint64_t>(digit);
    }
    return out;
}

std::optional<int64_t> orderTimeMsFromClientOrderId(std::string_view clientOrderId) {
    if (!startsWith(clientOrderId, "gtp_")) {
        return std::nullopt;
    }
    const size_t tailSep = clientOrderId.rfind('_');
    if (tailSep == std::string_view::npos || tailSep <= 4) {
        return std::nullopt;
    }
    const size_t tsSep = clientOrderId.rfind('_', tailSep - 1);
    if (tsSep == std::string_view::npos || tsSep + 1 >= tailSep) {
        return std::nullopt;
    }
    const auto ts36 = clientOrderId.substr(tsSep + 1, tailSep - tsSep - 1);
    if (ts36.size() < 6 || ts36.size() > 13) {
        return std::nullopt;
    }
    const auto decoded = fromBase36(ts36);
    if (!decoded.has_value() || *decoded > static_cast<std::uint64_t>(std::numeric_limits<int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<int64_t>(*decoded);
}

struct NormalizedClientIdSymbol {
    std::string value;
    bool truncated{false};
};

NormalizedClientIdSymbol normalizeClientIdSymbol(std::string_view symbol) {
    std::string out;
    out.reserve(12);
    bool truncated = false;
    for (size_t i = 0; i < symbol.size(); ++i) {
        const char c = symbol[i];
        const auto uc = static_cast<unsigned char>(c);
        char normalized = static_cast<char>(std::toupper(uc));
        const bool accepted =
            (normalized >= 'A' && normalized <= 'Z') ||
            (normalized >= '0' && normalized <= '9') ||
            normalized == '_' ||
            normalized == '-';
        if (!accepted) {
            continue;
        }
        out.push_back(normalized);
        if (out.size() >= 12) {
            truncated = (i + 1 < symbol.size());
            break;
        }
    }
    return {.value = std::move(out), .truncated = truncated};
}

bool shouldSkipFreshStaleCancel(
    const NormalOrderSnapshot& order,
    std::chrono::seconds staleSelfOwnedCancelGrace,
    std::chrono::system_clock::time_point now) {
    if (staleSelfOwnedCancelGrace.count() <= 0) {
        return false;
    }

    const int64_t orderTimeMs = order.updateTime > 0
        ? order.updateTime
        : (order.time > 0 ? order.time : orderTimeMsFromClientOrderId(order.clientOrderId).value_or(0));
    if (orderTimeMs <= 0) {
        return false;
    }

    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    if (orderTimeMs >= nowMs) {
        return true;
    }
    const auto ageMs = nowMs - orderTimeMs;
    return ageMs < std::chrono::duration_cast<std::chrono::milliseconds>(staleSelfOwnedCancelGrace).count();
}

std::optional<std::string> makeGlobalTpClientOrderId(std::string_view symbol, std::uint32_t sequence) {
    const auto normalized = normalizeClientIdSymbol(symbol);
    if (normalized.value.empty()) {
        return std::nullopt;
    }
    if (normalized.truncated) {
        Logger::instance().log(
            LogLevel::Warning,
            "take-profit reconcile client-id symbol truncated symbol=" + quoteString(symbol) +
                " normalized=" + quoteString(normalized.value));
    }

    std::uint32_t symbolHash = 2166136261U;
    for (char c : symbol) {
        const auto uc = static_cast<unsigned char>(c);
        const char up = static_cast<char>(std::toupper(uc));
        symbolHash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(up));
        symbolHash *= 16777619U;
    }
    // IN-6: widen the symbol fingerprint to 4 base36 chars (~1.68M buckets vs the
    // previous 1296) so two distinct symbols are far less likely to collide on the
    // same-millisecond client-id and have a take-profit adopted/cancelled on the
    // wrong symbol. The id stays under the 36-char cap (<=12 symbol + 4 + 8 ts + 2
    // seq + delimiters).
    constexpr std::uint64_t kFingerprintBuckets = 36ULL * 36ULL * 36ULL * 36ULL;
    constexpr std::size_t kFingerprintWidth = 4;
    std::string symbolFingerprint = toBase36(static_cast<std::uint64_t>(symbolHash) % kFingerprintBuckets);
    if (symbolFingerprint.size() < kFingerprintWidth) {
        symbolFingerprint = std::string(kFingerprintWidth - symbolFingerprint.size(), '0') + symbolFingerprint;
    } else if (symbolFingerprint.size() > kFingerprintWidth) {
        symbolFingerprint = symbolFingerprint.substr(symbolFingerprint.size() - kFingerprintWidth);
    }

    const auto nowMsRaw = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    const auto nowMs = nowMsRaw > 0 ? static_cast<std::uint64_t>(nowMsRaw) : 0ULL;
    std::string seq36 = toBase36(sequence % (36U * 36U));
    if (seq36.size() < 2) {
        seq36 = std::string(2 - seq36.size(), '0') + seq36;
    } else if (seq36.size() > 2) {
        seq36 = seq36.substr(seq36.size() - 2);
    }

    return "gtp_" + normalized.value + "_" + symbolFingerprint + "_" + toBase36(nowMs) + "_" + seq36;
}

TrackedPosition recoveredPositionFromSnapshot(
    const Position& position,
    std::chrono::seconds recoveredMaxHoldDuration) {
    TrackedPosition tracked;
    tracked.symbol = position.symbol;
    tracked.direction = directionFromPosition(position.positionAmt);
    tracked.entryPrice = position.entryPrice;
    tracked.quantity = std::abs(position.positionAmt);
    tracked.activeLeverage = position.leverage;
    tracked.openedAt = std::chrono::system_clock::now();
    tracked.maxHoldDuration = recoveredMaxHoldDuration;
    tracked.openingInFlight = false;
    tracked.recoveredFromSnapshot = true;
    return tracked;
}

enum class TrackerReadyState {
    Ready,
    OpeningInFlight
};

TrackerReadyState ensureTrackerReadyForPosition(
    PositionTracker& tracker,
    const Position& position,
    std::chrono::seconds recoveredMaxHoldDuration) {
    const auto existing = tracker.bySymbol(position.symbol);
    if (existing.has_value()) {
        return existing->openingInFlight ? TrackerReadyState::OpeningInFlight : TrackerReadyState::Ready;
    }
    if (tracker.addRecovered(recoveredPositionFromSnapshot(position, recoveredMaxHoldDuration))) {
        return TrackerReadyState::Ready;
    }
    const auto recheck = tracker.bySymbol(position.symbol);
    if (!recheck.has_value() || recheck->openingInFlight) {
        return TrackerReadyState::OpeningInFlight;
    }
    return TrackerReadyState::Ready;
}

} // namespace

TakeProfitReconciler::TakeProfitReconciler(
    TakeProfitReconcilerConfig config,
    IOrdersPort& orders,
    PositionTracker& tracker,
    SymbolInfoResolver symbolInfoResolver,
    double takeProfitPercent,
    std::chrono::seconds recoveredMaxHoldDuration,
    std::chrono::seconds staleSelfOwnedCancelGrace)
    : m_config(config),
      m_orders(orders),
      m_tracker(tracker),
      m_symbolInfoResolver(std::move(symbolInfoResolver)),
      m_takeProfitPercent(takeProfitPercent),
      m_recoveredMaxHoldDuration(recoveredMaxHoldDuration),
      m_staleSelfOwnedCancelGrace(staleSelfOwnedCancelGrace) {}

boost::asio::awaitable<void> TakeProfitReconciler::reconcileOnce(const account::AccountSnapshot& snapshot) {
    if (!m_config.enabled || m_takeProfitPercent <= 0.0) {
        co_return;
    }
    (void)m_config.priceTickTolerance;
    (void)m_config.quantityStepTolerance;

    const auto openOrdersResult = co_await m_orders.openNormalOrders(std::nullopt);
    if (!openOrdersResult) {
        Logger::instance().log(
            LogLevel::Warning,
            "take-profit reconcile open-orders failed reason=" +
                quoteString(openOrdersResult.error().toString()));
        co_return;
    }

    const auto& openOrders = *openOrdersResult;
    std::unordered_map<std::string, std::vector<const NormalOrderSnapshot*>> ordersBySymbol;
    ordersBySymbol.reserve(openOrders.size());
    for (const auto& order : openOrders) {
        ordersBySymbol[order.symbol].push_back(&order);
    }

    auto livePositions = livePositionsFromSnapshot(snapshot);
    const auto now = std::chrono::system_clock::now();
    std::unordered_set<std::string> liveSymbols;
    liveSymbols.reserve(livePositions.size());
    for (const Position* position : livePositions) {
        liveSymbols.insert(position->symbol);
    }

    int orderBudget = std::max(0, m_config.maxOrdersPerCycle);

    for (const auto& order : openOrders) {
        if (!startsWith(order.clientOrderId, "gtp_")) {
            continue;
        }
        if (liveSymbols.contains(order.symbol)) {
            continue;
        }
        if (shouldSkipFreshStaleCancel(order, m_staleSelfOwnedCancelGrace, now)) {
            continue;
        }
        if (orderBudget <= 0) {
            break;
        }

        bool cancelOk = false;
        if (order.orderId > 0) {
            const auto cancelResult = co_await m_orders.cancelNormalByOrderId(order.symbol, order.orderId);
            cancelOk = cancelResult.has_value();
            if (!cancelOk) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "take-profit reconcile stale cancel failed symbol=" + order.symbol +
                        " order_id=" + std::to_string(order.orderId) +
                        " reason=" + quoteString(cancelResult.error().toString()));
            }
        } else if (!order.clientOrderId.empty()) {
            const auto cancelResult =
                co_await m_orders.cancelNormalByClientOrderId(order.symbol, order.clientOrderId);
            cancelOk = cancelResult.has_value();
            if (!cancelOk) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "take-profit reconcile stale cancel failed symbol=" + order.symbol +
                        " client_order_id=" + quoteString(order.clientOrderId) +
                        " reason=" + quoteString(cancelResult.error().toString()));
            }
        }

        --orderBudget;
        if (cancelOk) {
            Logger::instance().log(
                LogLevel::Info,
                "take-profit reconcile cancelled stale self-owned tp symbol=" + order.symbol +
                    " client_order_id=" + quoteString(order.clientOrderId));
        }
    }

    std::uint32_t clientOrderSequence = 0;
    int deferredByBudget = 0;

    for (const Position* positionPtr : livePositions) {
        const Position& position = *positionPtr;
        const auto direction = directionFromPosition(position.positionAmt);
        if (direction == strategy::Signal::Direction::None) {
            continue;
        }

        const auto tracked = m_tracker.bySymbol(position.symbol);
        if (tracked.has_value() && tracked->openingInFlight) {
            continue;
        }

        if (position.leverage <= 0) {
            Logger::instance().log(
                LogLevel::Warning,
                "take-profit reconcile skipped symbol=" + position.symbol +
                    " reason=" + quoteString("invalid leverage <= 0"));
            continue;
        }

        std::vector<const NormalOrderSnapshot*> candidates;
        const auto ordersIt = ordersBySymbol.find(position.symbol);
        if (ordersIt != ordersBySymbol.end()) {
            candidates.reserve(ordersIt->second.size());
            for (const NormalOrderSnapshot* candidate : ordersIt->second) {
                if (!isLiveOrderStatus(candidate->status)) {
                    continue;
                }
                if (candidate->type != OrderType::Limit) {
                    continue;
                }
                if (!candidate->reduceOnly) {
                    continue;
                }
                if (candidate->side != closeSide(direction)) {
                    continue;
                }
                if (remainingQuantity(*candidate) <= 0.0) {
                    continue;
                }
                candidates.push_back(candidate);
            }
        }

        const std::string legacyPrefix = position.symbol + "_tp_";
        const NormalOrderSnapshot* ownedCandidate = nullptr;
        double ownedRemainingQty = -1.0;
        bool hasExternalCoverage = false;

        for (const NormalOrderSnapshot* candidate : candidates) {
            const bool selfOwned = startsWith(candidate->clientOrderId, "gtp_");
            const bool legacyOwned = startsWith(candidate->clientOrderId, legacyPrefix);
            if (selfOwned || legacyOwned) {
                const double remainingQty = remainingQuantity(*candidate);
                if (remainingQty > ownedRemainingQty) {
                    ownedRemainingQty = remainingQty;
                    ownedCandidate = candidate;
                }
                continue;
            }
            hasExternalCoverage = true;
        }

        if (ownedCandidate != nullptr) {
            if (ensureTrackerReadyForPosition(m_tracker, position, m_recoveredMaxHoldDuration) !=
                TrackerReadyState::Ready) {
                continue;
            }
            (void)m_tracker.refreshFromSnapshot(
                position.symbol,
                position.entryPrice,
                std::abs(position.positionAmt),
                position.leverage);
            if (!m_tracker.updateTakeProfit(
                position.symbol,
                ownedCandidate->orderId,
                ownedCandidate->clientOrderId)) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "take-profit reconcile adopt tracker update skipped symbol=" + position.symbol +
                        " client_order_id=" + quoteString(ownedCandidate->clientOrderId));
            }
            continue;
        }

        if (hasExternalCoverage) {
            continue;
        }

        if (orderBudget <= 0) {
            ++deferredByBudget;
            continue;
        }

        const auto symbolMeta = m_symbolInfoResolver(position.symbol);
        if (!symbolMeta.has_value() || symbolMeta->stepSize <= 0.0 || symbolMeta->tickSize <= 0.0) {
            Logger::instance().log(
                LogLevel::Warning,
                "take-profit reconcile skipped symbol=" + position.symbol +
                    " reason=" + quoteString("missing tick/step size"));
            continue;
        }
        if (position.entryPrice <= 0.0) {
            Logger::instance().log(
                LogLevel::Warning,
                "take-profit reconcile skipped symbol=" + position.symbol +
                    " reason=" + quoteString("entry price <= 0"));
            continue;
        }

        const double tpDistance =
            position.entryPrice * m_takeProfitPercent / (100.0 * static_cast<double>(position.leverage));
        if (tpDistance <= 0.0 || !std::isfinite(tpDistance)) {
            Logger::instance().log(
                LogLevel::Warning,
                "take-profit reconcile skipped symbol=" + position.symbol +
                    " reason=" + quoteString("invalid tp distance"));
            continue;
        }

        const double tpPriceValue = direction == strategy::Signal::Direction::Long
            ? position.entryPrice + tpDistance
            : position.entryPrice - tpDistance;
        const auto tpRounding = direction == strategy::Signal::Direction::Long
            ? PriceRounding::Down
            : PriceRounding::Up;
        const auto tpPrice = priceToTickDecimal(tpPriceValue, symbolMeta->tickSize, symbolMeta->tickSizeRaw, tpRounding);
        if (!tpPrice.has_value()) {
            Logger::instance().log(
                LogLevel::Warning,
                "take-profit reconcile skipped symbol=" + position.symbol +
                    " reason=" + quoteString("invalid tp price decimal"));
            continue;
        }

        const auto tpQty = quantityToStepDecimal(std::abs(position.positionAmt), symbolMeta->stepSize, symbolMeta->stepSizeRaw);
        if (!tpQty.has_value()) {
            Logger::instance().log(
                LogLevel::Warning,
                "take-profit reconcile skipped symbol=" + position.symbol +
                    " reason=" + quoteString("invalid tp quantity decimal"));
            continue;
        }

        const auto clientOrderId = makeGlobalTpClientOrderId(position.symbol, clientOrderSequence++);
        if (!clientOrderId.has_value()) {
            Logger::instance().log(
                LogLevel::Warning,
                "take-profit reconcile skipped symbol=" + position.symbol +
                    " reason=" + quoteString("could not build gtp client order id"));
            continue;
        }
        if (ensureTrackerReadyForPosition(m_tracker, position, m_recoveredMaxHoldDuration) !=
            TrackerReadyState::Ready) {
            continue;
        }

        const auto placementResult = co_await m_orders.limit(LimitOrderDraft{
            .symbol = position.symbol,
            .side = closeSide(direction),
            .quantity = *tpQty,
            .price = *tpPrice,
            .timeInForce = TimeInForce::GTC,
            .positionSide = PositionSide::Both,
            .reduceOnly = true,
            .clientOrderId = *clientOrderId,
        });
        --orderBudget;

        if (!placementResult) {
            Logger::instance().log(
                LogLevel::Warning,
                "take-profit reconcile placement failed symbol=" + position.symbol +
                    " client_order_id=" + quoteString(*clientOrderId) +
                    " reason=" + quoteString(placementResult.error().toString()));
            continue;
        }
        if (placementResult->state != PlacementState::Accepted) {
            Logger::instance().log(
                LogLevel::Warning,
                "take-profit reconcile placement rejected symbol=" + position.symbol +
                    " client_order_id=" + quoteString(*clientOrderId) +
                    " code=" + std::to_string(placementResult->binanceCode.value_or(-1)) +
                    " message=" + quoteString(placementResult->binanceMessage.value_or("unknown")));
            continue;
        }

        const int64_t orderId = placementResult->orderId.value_or(0);
        const std::string appliedClientOrderId = placementResult->clientOrderId.empty()
            ? *clientOrderId
            : placementResult->clientOrderId;
        if (!m_tracker.updateTakeProfit(position.symbol, orderId, appliedClientOrderId)) {
            Logger::instance().log(
                LogLevel::Warning,
                "take-profit reconcile placement tracker update skipped symbol=" + position.symbol +
                    " client_order_id=" + quoteString(appliedClientOrderId) +
                    " order_id=" + std::to_string(orderId));
        }
    }

    if (deferredByBudget > 0) {
        Logger::instance().log(
            LogLevel::Info,
            "take-profit reconcile budget exhausted deferred_symbols=" + std::to_string(deferredByBudget));
    }

    co_return;
}

} // namespace engine
