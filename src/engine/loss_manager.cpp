#include "engine/loss_manager.h"

#include "engine/price_filter.h"
#include "engine/signal_engine.h"
#include "logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>

namespace engine {

namespace {

constexpr double kEpsilon = 1e-9;

std::string makeLossManagerTpClientId(std::string_view symbol) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "lm_tp_" + std::string(symbol) + "_" + std::to_string(now);
}

std::string makeLossManagerSlClientId(std::string_view symbol) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "lm_sl_" + std::string(symbol) + "_" + std::to_string(now);
}

bool nearEqual(double lhs, double rhs, double epsilon) {
    return std::abs(lhs - rhs) <= std::max(epsilon, std::max(std::abs(lhs), std::abs(rhs)) * 1e-9);
}

} // namespace

LossManager::LossManager(
    LossManagerConfig config,
    IOrdersPort& orders,
    IOrderCapPort& orderCap,
    IExposurePort& exposure,
    PositionTracker& tracker,
    SymbolInfoResolver symbolInfoResolver)
    : m_config(config),
      m_orders(orders),
      m_orderCap(orderCap),
      m_exposure(exposure),
      m_tracker(tracker),
      m_symbolInfoResolver(std::move(symbolInfoResolver)),
      m_enabled(config.enabled) {
    std::string reason;
    if (!validateConfig(m_config, &reason)) {
        m_enabled = false;
        Logger::instance().log(
            LogLevel::Error,
            "loss manager disabled because config is invalid reason=" + quoteString(reason));
    }
}

bool LossManager::validateConfig(const LossManagerConfig& config, std::string* reason) {
    auto fail = [reason](std::string msg) {
        if (reason) {
            *reason = std::move(msg);
        }
        return false;
    };

    if (config.roiBeThreshold >= 0.0) {
        return fail("roi_be_threshold must be < 0");
    }
    if (config.roiDcaThreshold > config.roiBeThreshold) {
        return fail("roi_dca_threshold must be <= roi_be_threshold");
    }
    if (config.maxDcaCount < 0) {
        return fail("max_dca_count must be >= 0");
    }
    if (config.fallbackTakerFeeRate < 0.0 || config.fallbackTakerFeeRate >= 0.5) {
        return fail("fallback_taker_fee_rate is out of range");
    }
    if (config.dcaPendingTimeoutCycles <= 0) {
        return fail("dca_pending_timeout_cycles must be > 0");
    }
    return true;
}

boost::asio::awaitable<void> LossManager::evaluate(
    const account::AccountSnapshot& snapshot,
    double availableBalance) {
    if (!m_enabled) {
        co_return;
    }

    const auto trackedPositions = m_tracker.all();
    std::unordered_map<std::string, bool> trackedSymbols;
    trackedSymbols.reserve(trackedPositions.size());
    for (const auto& tracked : trackedPositions) {
        trackedSymbols[tracked.symbol] = true;
    }
    for (auto it = m_states.begin(); it != m_states.end();) {
        if (trackedSymbols.find(it->first) == trackedSymbols.end()) {
            it = m_states.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& tracked : trackedPositions) {
        auto livePosition = findLivePosition(snapshot, tracked.symbol);
        if (!livePosition.has_value()) {
            m_states.erase(tracked.symbol);
            continue;
        }
        co_await processTrackedPosition(snapshot, availableBalance, tracked, *livePosition);
    }
    co_return;
}

boost::asio::awaitable<void> LossManager::processTrackedPosition(
    const account::AccountSnapshot& snapshot,
    double availableBalance,
    const TrackedPosition& tracked,
    const Position& livePosition) {
    if (!isSameSide(livePosition, tracked)) {
        co_return;
    }
    if (!isStillTracked(tracked)) {
        m_states.erase(tracked.symbol);
        co_return;
    }

    const auto symbolInfo = m_symbolInfoResolver ? m_symbolInfoResolver(tracked.symbol) : std::nullopt;
    const double stepSize = symbolInfo.has_value() ? symbolInfo->stepSize : 0.0;
    const double qtyTolerance = stepSize > 0.0 ? stepSize * 0.5 : kEpsilon;
    const double absLiveQty = std::abs(livePosition.positionAmt);
    m_tracker.refreshPositionView(tracked.symbol, livePosition.entryPrice, absLiveQty);

    auto& state = m_states[tracked.symbol];
    if (state.originalQuantity <= 0.0) {
        state.originalQuantity = tracked.quantity > 0.0 ? tracked.quantity : absLiveQty;
        if (tracked.recoveredFromSnapshot && !m_config.allowDcaOnRecoveredPositions) {
            state.dcaCount = m_config.maxDcaCount;
        }
    }

    if (state.dcaPending) {
        const double expectedAbsQty = std::abs(state.positionAmtBeforeDca) + state.originalQuantity;
        if (absLiveQty + qtyTolerance >= expectedAbsQty) {
            state.dcaCount += 1;
            state.dcaPending = false;
            state.dcaPendingCycles = 0;
            state.beCurrent = false;
            m_tracker.refreshPositionView(tracked.symbol, livePosition.entryPrice, absLiveQty);
            (void)co_await refreshStopLossAfterDca(tracked, livePosition, state);
        } else {
            if (state.dcaPendingCycles <= m_config.dcaPendingTimeoutCycles) {
                ++state.dcaPendingCycles;
            }
            if (state.dcaPendingCycles == m_config.dcaPendingTimeoutCycles + 1) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "loss manager dca pending timeout symbol=" + tracked.symbol +
                        " order_id=" + std::to_string(state.dcaOrderId) +
                        " client_order_id=" + quoteString(state.dcaClientOrderId) +
                        " amount_before=" + std::to_string(state.positionAmtBeforeDca) +
                        " entry_before=" + std::to_string(state.entryPriceBeforeDca) +
                        " entry_now=" + std::to_string(livePosition.entryPrice) +
                        " amount_now=" + std::to_string(absLiveQty));
            }
            co_return;
        }
    }

    if (state.beCurrent) {
        if (!nearEqual(state.beEntryPrice, livePosition.entryPrice, 1e-8) ||
            !nearEqual(state.bePositionAmt, absLiveQty, qtyTolerance)) {
            state.beCurrent = false;
        }
    }

    const double roi = calcRoi(livePosition);
    if (roi <= m_config.roiDcaThreshold &&
        !state.dcaPending &&
        m_config.maxDcaCount > 0 &&
        state.dcaCount < m_config.maxDcaCount) {
        const bool dcaPlaced = co_await placeDca(snapshot, availableBalance, tracked, livePosition, state);
        if (dcaPlaced) {
            co_return;
        }
    }

    if (roi <= m_config.roiBeThreshold) {
        (void)co_await applyBreakEvenTakeProfit(tracked, livePosition, state);
    }
    co_return;
}

boost::asio::awaitable<bool> LossManager::applyBreakEvenTakeProfit(
    const TrackedPosition& tracked,
    const Position& livePosition,
    LossManagerState& state) {
    if (state.beCurrent) {
        co_return true;
    }

    const auto symbolInfo = m_symbolInfoResolver ? m_symbolInfoResolver(tracked.symbol) : std::nullopt;
    const double tickSize = symbolInfo.has_value() ? symbolInfo->tickSize : 0.0;
    const double stepSize = symbolInfo.has_value() ? symbolInfo->stepSize : 0.0;
    const std::string tickSizeRaw = symbolInfo.has_value() ? symbolInfo->tickSizeRaw : std::string{};
    const std::string stepSizeRaw = symbolInfo.has_value() ? symbolInfo->stepSizeRaw : std::string{};

    const auto be = resolveBreakEvenPrice(livePosition);
    if (!be.has_value() || *be <= 0.0) {
        Logger::instance().log(
            LogLevel::Warning,
            "loss manager skip BE update because be price is unavailable symbol=" + tracked.symbol);
        co_return false;
    }

    const auto closeQty = quantityToStepDecimal(std::abs(livePosition.positionAmt), stepSize, stepSizeRaw);
    if (!closeQty) {
        Logger::instance().log(
            LogLevel::Warning,
            "loss manager skip BE update because quantity rounding failed symbol=" + tracked.symbol);
        co_return false;
    }

    const auto beRounding = livePosition.positionAmt > 0.0 ? PriceRounding::Up : PriceRounding::Down;
    const auto bePrice = priceToTickDecimal(*be, tickSize, tickSizeRaw, beRounding);
    if (!bePrice) {
        Logger::instance().log(
            LogLevel::Warning,
            "loss manager skip BE update because price rounding failed symbol=" + tracked.symbol);
        co_return false;
    }

    const OrderSide closeSide = closeSideFor(livePosition);
    if (tracked.tpOrderId > 0 || !tracked.tpClientOrderId.empty()) {
        if (!isStillTracked(tracked)) {
            co_return false;
        }
        auto amendResult = co_await m_orders.amendLimitOrder(AmendLimitOrderDraft{
            .identity = NormalOrderIdentity{
                .symbol = tracked.symbol,
                .orderId = tracked.tpOrderId > 0 ? std::optional<int64_t>{tracked.tpOrderId} : std::nullopt,
                .clientOrderId = !tracked.tpClientOrderId.empty() ? std::optional<ClientOrderId>{tracked.tpClientOrderId}
                                                                  : std::nullopt,
            },
            .side = closeSide,
            .quantity = *closeQty,
            .price = *bePrice,
        });
        if (amendResult) {
            const auto nextTpOrderId = amendResult->orderId;
            const auto nextTpClientOrderId = amendResult->clientOrderId;
            m_tracker.updateTakeProfit(tracked.symbol, nextTpOrderId, nextTpClientOrderId);
            state.beCurrent = true;
            state.beEntryPrice = livePosition.entryPrice;
            state.bePositionAmt = std::abs(livePosition.positionAmt);
            state.bePrice = bePrice->toDouble();
            Logger::instance().log(
                LogLevel::Info,
                "loss manager BE amend succeeded symbol=" + tracked.symbol +
                    " order_id=" + std::to_string(nextTpOrderId) +
                    " client_order_id=" + quoteString(nextTpClientOrderId) +
                    " be_price=" + std::string(bePrice->value()));
            co_return true;
        }

        if (isOrderNotFound(amendResult.error())) {
            m_tracker.clearTakeProfit(tracked.symbol);
        } else {
            Logger::instance().log(
                LogLevel::Warning,
                "loss manager BE amend failed symbol=" + tracked.symbol +
                    " reason=" + quoteString(amendResult.error().toString()));
            co_return false;
        }
    }

    const auto tpClientOrderId = makeLossManagerTpClientId(tracked.symbol);
    if (!isStillTracked(tracked)) {
        co_return false;
    }
    auto placeResult = co_await m_orders.limit(LimitOrderDraft{
        .symbol = tracked.symbol,
        .side = closeSide,
        .quantity = *closeQty,
        .price = *bePrice,
        .timeInForce = TimeInForce::GTC,
        .positionSide = PositionSide::Both,
        .reduceOnly = true,
        .clientOrderId = tpClientOrderId,
    });
    if (!placeResult) {
        Logger::instance().log(
            LogLevel::Warning,
            "loss manager BE placement failed symbol=" + tracked.symbol +
                " reason=" + quoteString(placeResult.error().toString()));
        co_return false;
    }
    if (placeResult->state != PlacementState::Accepted) {
        Logger::instance().log(
            LogLevel::Warning,
            "loss manager BE placement rejected symbol=" + tracked.symbol);
        co_return false;
    }

    m_tracker.updateTakeProfit(
        tracked.symbol,
        placeResult->orderId.value_or(0),
        placeResult->clientOrderId.empty() ? tpClientOrderId : placeResult->clientOrderId);
    state.beCurrent = true;
    state.beEntryPrice = livePosition.entryPrice;
    state.bePositionAmt = std::abs(livePosition.positionAmt);
    state.bePrice = bePrice->toDouble();
    Logger::instance().log(
        LogLevel::Info,
        "loss manager BE placement succeeded symbol=" + tracked.symbol +
            " order_id=" + std::to_string(placeResult->orderId.value_or(0)) +
            " client_order_id=" + quoteString(
                placeResult->clientOrderId.empty() ? tpClientOrderId : placeResult->clientOrderId) +
            " be_price=" + std::string(bePrice->value()));
    co_return true;
}

boost::asio::awaitable<bool> LossManager::placeDca(
    const account::AccountSnapshot& snapshot,
    double availableBalance,
    const TrackedPosition& tracked,
    const Position& livePosition,
    LossManagerState& state) {
    const auto symbolInfo = m_symbolInfoResolver ? m_symbolInfoResolver(tracked.symbol) : std::nullopt;
    const double stepSize = symbolInfo.has_value() ? symbolInfo->stepSize : 0.0;
    const std::string stepSizeRaw = symbolInfo.has_value() ? symbolInfo->stepSizeRaw : std::string{};

    const auto dcaQty = quantityToStepDecimal(state.originalQuantity, stepSize, stepSizeRaw);
    if (!dcaQty) {
        Logger::instance().log(
            LogLevel::Warning,
            "loss manager skip DCA because quantity rounding failed symbol=" + tracked.symbol);
        co_return false;
    }

    const double refPrice = livePosition.markPrice > 0.0 ? livePosition.markPrice : livePosition.entryPrice;
    const double dcaNotional = state.originalQuantity * std::abs(refPrice);

    const auto cap = m_orderCap.check(dcaNotional, snapshot, m_tracker);
    if (cap.decision == OrderCapDecision::Block) {
        Logger::instance().log(
            LogLevel::Info,
            "loss manager DCA blocked by order cap symbol=" + tracked.symbol +
                " reason=" + quoteString(cap.reason));
        co_return false;
    }

    const auto exposure = m_exposure.check(
        tracked.symbol,
        directionFor(livePosition),
        dcaNotional,
        m_tracker,
        snapshot,
        availableBalance);
    if (exposure.decision != ExposureDecision::Allow) {
        Logger::instance().log(
            LogLevel::Info,
            "loss manager DCA blocked by exposure symbol=" + tracked.symbol +
                " reason=" + quoteString(exposure.reason));
        co_return false;
    }

    if (!isStillTracked(tracked)) {
        co_return false;
    }
    if (tracked.slOrderId <= 0 && tracked.slClientOrderId.empty()) {
        Logger::instance().log(
            LogLevel::Warning,
            "loss manager skip DCA because stop-loss protection is not tracked symbol=" + tracked.symbol);
        co_return false;
    }
    auto dcaResult = co_await m_orders.market(MarketOrderDraft{
        .symbol = tracked.symbol,
        .side = dcaSideFor(livePosition),
        .quantity = *dcaQty,
        .positionSide = PositionSide::Both,
        .reduceOnly = false,
        .responseType = ResponseType::RESULT,
    });
    if (!dcaResult || dcaResult->state != PlacementState::Accepted) {
        if (!dcaResult) {
            Logger::instance().log(
                LogLevel::Warning,
                "loss manager DCA failed symbol=" + tracked.symbol +
                    " reason=" + quoteString(dcaResult.error().toString()));
        } else {
            Logger::instance().log(
                LogLevel::Warning,
                "loss manager DCA rejected symbol=" + tracked.symbol +
                    " state=" + std::to_string(static_cast<int>(dcaResult->state)));
        }
        co_return false;
    }
    if (!isStillTracked(tracked)) {
        Logger::instance().log(
            LogLevel::Warning,
            "loss manager DCA accepted but tracker was removed symbol=" + tracked.symbol +
                " order_id=" + std::to_string(dcaResult->orderId.value_or(0)) +
                " client_order_id=" + quoteString(dcaResult->clientOrderId));
        co_return false;
    }

    state.dcaPending = true;
    state.dcaPendingCycles = 0;
    state.dcaOrderId = dcaResult->orderId.value_or(0);
    state.dcaClientOrderId = dcaResult->clientOrderId;
    state.positionAmtBeforeDca = std::abs(livePosition.positionAmt);
    state.entryPriceBeforeDca = livePosition.entryPrice;
    state.beCurrent = false;
    Logger::instance().log(
        LogLevel::Info,
        "loss manager DCA accepted symbol=" + tracked.symbol +
            " order_id=" + std::to_string(state.dcaOrderId) +
            " client_order_id=" + quoteString(state.dcaClientOrderId) +
            " quantity=" + std::string(dcaQty->value()));
    co_return true;
}

boost::asio::awaitable<bool> LossManager::refreshStopLossAfterDca(
    const TrackedPosition& tracked,
    const Position& livePosition,
    const LossManagerState& state) {
    const auto latest = m_tracker.bySymbol(tracked.symbol);
    if (!latest.has_value() || latest->openedAt != tracked.openedAt) {
        co_return false;
    }
    if (latest->slOrderId <= 0 && latest->slClientOrderId.empty()) {
        Logger::instance().log(
            LogLevel::Warning,
            "loss manager skip DCA SL refresh because stop-loss protection is not tracked symbol=" + tracked.symbol);
        co_return false;
    }
    if (latest->currentTrailLevel <= 0.0 || state.entryPriceBeforeDca <= 0.0 || livePosition.entryPrice <= 0.0) {
        Logger::instance().log(
            LogLevel::Warning,
            "loss manager skip DCA SL refresh because stop level is unavailable symbol=" + tracked.symbol);
        co_return false;
    }

    const bool isLong = livePosition.positionAmt > 0.0;
    const double originalStopDistance = isLong
        ? state.entryPriceBeforeDca - latest->currentTrailLevel
        : latest->currentTrailLevel - state.entryPriceBeforeDca;
    if (originalStopDistance <= 0.0) {
        Logger::instance().log(
            LogLevel::Warning,
            "loss manager skip DCA SL refresh because stop distance is invalid symbol=" + tracked.symbol);
        co_return false;
    }

    const double nextStopLevel = isLong
        ? livePosition.entryPrice - originalStopDistance
        : livePosition.entryPrice + originalStopDistance;
    if (nextStopLevel <= 0.0) {
        Logger::instance().log(
            LogLevel::Warning,
            "loss manager skip DCA SL refresh because adjusted stop is invalid symbol=" + tracked.symbol);
        co_return false;
    }

    const auto symbolInfo = m_symbolInfoResolver ? m_symbolInfoResolver(tracked.symbol) : std::nullopt;
    const double tickSize = symbolInfo.has_value() ? symbolInfo->tickSize : 0.0;
    const double stepSize = symbolInfo.has_value() ? symbolInfo->stepSize : 0.0;
    const std::string tickSizeRaw = symbolInfo.has_value() ? symbolInfo->tickSizeRaw : std::string{};
    const std::string stepSizeRaw = symbolInfo.has_value() ? symbolInfo->stepSizeRaw : std::string{};
    const auto closeQty = quantityToStepDecimal(std::abs(livePosition.positionAmt), stepSize, stepSizeRaw);
    const auto trigger = priceToTickDecimal(
        nextStopLevel,
        tickSize,
        tickSizeRaw,
        isLong ? PriceRounding::Up : PriceRounding::Down);
    if (!closeQty || !trigger) {
        Logger::instance().log(
            LogLevel::Warning,
            "loss manager skip DCA SL refresh because decimal conversion failed symbol=" + tracked.symbol);
        co_return false;
    }

    const auto clientAlgoId = makeLossManagerSlClientId(tracked.symbol);
    auto protection = co_await m_orders.protection(ProtectionOrderDraft{
        .symbol = tracked.symbol,
        .positionSide = PositionSide::Both,
        .closeSide = closeSideFor(livePosition),
        .kind = ProtectionKind::StopLoss,
        .triggerPrice = *trigger,
        .closeQuantity = *closeQty,
        .clientAlgoId = clientAlgoId,
    });
    if (!protection || protection->state != PlacementState::Accepted) {
        Logger::instance().log(
            LogLevel::Warning,
            "loss manager DCA SL refresh placement failed symbol=" + tracked.symbol);
        co_return false;
    }

    bool cancelSucceeded = false;
    if (latest->slOrderId > 0) {
        auto cancel = co_await m_orders.cancelAlgoByAlgoId(tracked.symbol, latest->slOrderId);
        cancelSucceeded = cancel.has_value();
    } else if (!latest->slClientOrderId.empty()) {
        auto cancel = co_await m_orders.cancelAlgoByClientAlgoId(tracked.symbol, latest->slClientOrderId);
        cancelSucceeded = cancel.has_value();
    }
    if (!cancelSucceeded) {
        Logger::instance().log(
            LogLevel::Warning,
            "loss manager DCA old SL cancel failed symbol=" + tracked.symbol);
    }

    m_tracker.updateStopLoss(
        tracked.symbol,
        protection->orderId.value_or(0),
        protection->clientOrderId.empty() ? clientAlgoId : protection->clientOrderId,
        trigger->toDouble());
    Logger::instance().log(
        LogLevel::Info,
        "loss manager DCA SL refreshed symbol=" + tracked.symbol +
            " order_id=" + std::to_string(protection->orderId.value_or(0)) +
            " client_order_id=" + quoteString(
                protection->clientOrderId.empty() ? clientAlgoId : protection->clientOrderId) +
            " stop_price=" + std::string(trigger->value()));
    co_return true;
}

double LossManager::calcRoi(const Position& pos) {
    if (pos.entryPrice <= 0.0 || std::abs(pos.positionAmt) <= kEpsilon || pos.leverage <= 0) {
        return 0.0;
    }
    const double direction = pos.positionAmt > 0.0 ? 1.0 : -1.0;
    return ((pos.markPrice - pos.entryPrice) / pos.entryPrice)
        * static_cast<double>(pos.leverage)
        * direction;
}

OrderSide LossManager::closeSideFor(const Position& pos) {
    return pos.positionAmt > 0.0 ? OrderSide::Sell : OrderSide::Buy;
}

OrderSide LossManager::dcaSideFor(const Position& pos) {
    return pos.positionAmt > 0.0 ? OrderSide::Buy : OrderSide::Sell;
}

strategy::Signal::Direction LossManager::directionFor(const Position& pos) {
    return pos.positionAmt > 0.0 ? strategy::Signal::Direction::Long : strategy::Signal::Direction::Short;
}

bool LossManager::isSameSide(const Position& pos, const TrackedPosition& tracked) {
    if (pos.positionAmt > 0.0) {
        return tracked.direction == strategy::Signal::Direction::Long;
    }
    if (pos.positionAmt < 0.0) {
        return tracked.direction == strategy::Signal::Direction::Short;
    }
    return false;
}

bool LossManager::isOrderNotFound(const BinanceError& err) {
    if (err.code == -2013 || err.code == -2011) {
        return true;
    }
    const auto message = err.message;
    return message.find("order does not exist") != std::string::npos ||
        message.find("unknown order") != std::string::npos;
}

std::string LossManager::quoteString(std::string_view value) {
    std::ostringstream out;
    out << std::quoted(std::string(value));
    return out.str();
}

double LossManager::fallbackBreakEven(double entryPrice, bool isLong, double takerFeeRate) {
    if (isLong) {
        return entryPrice * (1.0 + takerFeeRate) / (1.0 - takerFeeRate);
    }
    return entryPrice * (1.0 - takerFeeRate) / (1.0 + takerFeeRate);
}

std::optional<double> LossManager::resolveBreakEvenPrice(const Position& pos) const {
    if (pos.breakEvenPrice > 0.0) {
        return pos.breakEvenPrice;
    }
    if (pos.entryPrice <= 0.0) {
        return std::nullopt;
    }
    return fallbackBreakEven(pos.entryPrice, pos.positionAmt > 0.0, m_config.fallbackTakerFeeRate);
}

std::optional<Position> LossManager::findLivePosition(
    const account::AccountSnapshot& snapshot,
    std::string_view symbol) const {
    const auto& positions = snapshot.positions.has_value()
        ? *snapshot.positions
        : snapshot.account.positions;
    std::optional<Position> out;
    for (const auto& pos : positions) {
        if (pos.symbol != symbol || std::abs(pos.positionAmt) <= kEpsilon) {
            continue;
        }
        if (!out.has_value() || pos.positionSide == PositionSide::Both) {
            out = pos;
        }
    }
    return out;
}

bool LossManager::isStillTracked(const TrackedPosition& tracked) const {
    const auto current = m_tracker.bySymbol(tracked.symbol);
    if (!current.has_value()) {
        return false;
    }
    return current->openedAt == tracked.openedAt;
}

} // namespace engine
