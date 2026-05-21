#pragma once

#include "account/account_snapshot.h"
#include "engine/exposure_controller.h"
#include "engine/order_cap_controller.h"
#include "engine/position_tracker.h"
#include "types/market.h"

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace engine {

class IOrdersPort;

struct LossManagerConfig {
    bool enabled{false};
    double roiBeThreshold{-0.50};
    double roiDcaThreshold{-0.80};
    int maxDcaCount{2};
    double fallbackTakerFeeRate{0.0004};
    bool allowDcaOnRecoveredPositions{false};
    int dcaPendingTimeoutCycles{3};
};

class LossManager {
public:
    using SymbolInfoResolver = std::function<std::optional<ExchangeSymbol>(std::string_view)>;

    LossManager(
        LossManagerConfig config,
        IOrdersPort& orders,
        IOrderCapPort& orderCap,
        IExposurePort& exposure,
        PositionTracker& tracker,
        SymbolInfoResolver symbolInfoResolver);

    static bool validateConfig(const LossManagerConfig& config, std::string* reason = nullptr);

    bool enabled() const { return m_enabled; }
    boost::asio::awaitable<void> evaluate(
        const account::AccountSnapshot& snapshot,
        double availableBalance);

private:
    struct LossManagerState {
        double originalQuantity{0.0};
        int dcaCount{0};

        bool beCurrent{false};
        double beEntryPrice{0.0};
        double bePositionAmt{0.0};
        double bePrice{0.0};

        bool dcaPending{false};
        int dcaPendingCycles{0};
        int64_t dcaOrderId{0};
        std::string dcaClientOrderId;
        double positionAmtBeforeDca{0.0};
        double entryPriceBeforeDca{0.0};
    };

    boost::asio::awaitable<void> processTrackedPosition(
        const account::AccountSnapshot& snapshot,
        double availableBalance,
        const TrackedPosition& tracked,
        const Position& livePosition);
    boost::asio::awaitable<bool> applyBreakEvenTakeProfit(
        const TrackedPosition& tracked,
        const Position& livePosition,
        LossManagerState& state);
    boost::asio::awaitable<bool> placeDca(
        const account::AccountSnapshot& snapshot,
        double availableBalance,
        const TrackedPosition& tracked,
        const Position& livePosition,
        LossManagerState& state);
    boost::asio::awaitable<bool> refreshStopLossAfterDca(
        const TrackedPosition& tracked,
        const Position& livePosition,
        const LossManagerState& state);

    static double calcRoi(const Position& pos);
    static OrderSide closeSideFor(const Position& pos);
    static OrderSide dcaSideFor(const Position& pos);
    static strategy::Signal::Direction directionFor(const Position& pos);
    static bool isSameSide(const Position& pos, const TrackedPosition& tracked);
    static bool isOrderNotFound(const BinanceError& err);
    static std::string quoteString(std::string_view value);
    static double fallbackBreakEven(double entryPrice, bool isLong, double takerFeeRate);

    std::optional<double> resolveBreakEvenPrice(const Position& pos) const;
    std::optional<Position> findLivePosition(
        const account::AccountSnapshot& snapshot,
        std::string_view symbol) const;
    bool isStillTracked(const TrackedPosition& tracked) const;

    LossManagerConfig m_config;
    IOrdersPort& m_orders;
    IOrderCapPort& m_orderCap;
    IExposurePort& m_exposure;
    PositionTracker& m_tracker;
    SymbolInfoResolver m_symbolInfoResolver;
    std::unordered_map<std::string, LossManagerState> m_states;
    bool m_enabled{false};
};

} // namespace engine
