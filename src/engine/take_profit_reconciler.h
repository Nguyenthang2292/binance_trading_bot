#pragma once

#include "account/account_snapshot.h"
#include "types/market.h"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string_view>

namespace engine {

class IOrdersPort;
class PositionTracker;

struct TakeProfitReconcilerConfig {
    bool enabled{false};
    enum class Mode {
        AdoptOrPlace,
        EnforceGlobal
    };
    Mode mode{Mode::AdoptOrPlace};
    // Reserved for v2 amend/retarget logic. Intentionally unused in v1.
    int priceTickTolerance{1};
    // Reserved for v2 amend/retarget logic. Intentionally unused in v1.
    int quantityStepTolerance{1};
    int maxOrdersPerCycle{8};
};

class TakeProfitReconciler {
public:
    using SymbolInfoResolver = std::function<std::optional<ExchangeSymbol>(std::string_view)>;

    TakeProfitReconciler(
        TakeProfitReconcilerConfig config,
        IOrdersPort& orders,
        PositionTracker& tracker,
        SymbolInfoResolver symbolInfoResolver,
        double takeProfitPercent,
        std::chrono::seconds recoveredMaxHoldDuration,
        std::chrono::seconds staleSelfOwnedCancelGrace = std::chrono::seconds{0});

    boost::asio::awaitable<void> reconcileOnce(const account::AccountSnapshot& snapshot);

private:
    TakeProfitReconcilerConfig m_config;
    IOrdersPort& m_orders;
    PositionTracker& m_tracker;
    SymbolInfoResolver m_symbolInfoResolver;
    double m_takeProfitPercent{0.0};
    std::chrono::seconds m_recoveredMaxHoldDuration{std::chrono::hours(24)};
    std::chrono::seconds m_staleSelfOwnedCancelGrace{0};
};

} // namespace engine
