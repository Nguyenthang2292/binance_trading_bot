#pragma once

#include "account/account_service.h"
#include "engine/exposure_controller.h"
#include "engine/order_cap_controller.h"
#include "engine/gemini_filter.h"
#include "engine/loss_manager.h"
#include "engine/position_tracker.h"
#include "engine/trailing_stop_controller.h"
#include "engine/work_queue.h"
#include "orders/orders.h"
#include "risk/irisk_port.h"
#include "scanner/market_scanner.h"
#include "strategy/indicators/atr.h"
#include "strategy/strategy_registry.h"
#include "types/events.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine {

class IScannerPort {
public:
    virtual ~IScannerPort() = default;
    virtual const scanner::KlineCache& cache() const = 0;
    virtual std::vector<std::string> symbols() const = 0;
    virtual std::optional<ExchangeSymbol> symbolInfo(std::string_view symbol) const = 0;
    virtual boost::asio::io_context& ioContext() = 0;
};

class IAccountPort {
public:
    virtual ~IAccountPort() = default;
    virtual boost::asio::awaitable<account::AccountServiceResult<account::AccountSnapshot>> snapshot(
        account::AccountSnapshotRequest request = {}) = 0;
};

class IOrdersPort {
public:
    virtual ~IOrdersPort() = default;
    virtual boost::asio::awaitable<OrdersResult<NormalPlacementResult>> market(MarketOrderDraft draft) = 0;
    virtual boost::asio::awaitable<OrdersResult<NormalPlacementResult>> limit(LimitOrderDraft draft) = 0;
    virtual boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> amendLimitOrder(AmendLimitOrderDraft draft) = 0;
    virtual boost::asio::awaitable<OrdersResult<NormalPlacementResult>> protection(ProtectionOrderDraft draft) = 0;
    virtual boost::asio::awaitable<OrdersResult<NormalPlacementResult>> closeByMarket(CloseByMarketDraft draft) = 0;
    virtual boost::asio::awaitable<OrdersResult<LeverageResult>> setLeverage(Symbol symbol, int leverage) = 0;
    virtual boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelNormalByOrderId(Symbol symbol, int64_t orderId) = 0;
    virtual boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelNormalByClientOrderId(
        Symbol symbol,
        ClientOrderId clientOrderId) = 0;
    virtual boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelAlgoByAlgoId(Symbol symbol, int64_t algoId) = 0;
    virtual boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelAlgoByClientAlgoId(
        Symbol symbol,
        ClientAlgoId clientAlgoId) = 0;
};

class SignalEngine {
public:
    struct Config {
        double minNotional{1.0};
        double maxPositionNotionalXAvailableBalance{0.5};
        std::chrono::seconds positionCheckInterval{60};
        std::chrono::seconds trailingCheckInterval{300};
        bool placeStopLoss{true};
        bool monitorTrailingStops{true};
        LossManagerConfig lossManager;
    };

    SignalEngine(
        IScannerPort& scanner,
        strategy::StrategyRegistry& registry,
        IAccountPort& account,
        IOrdersPort& orders,
        IOrderCapPort& orderCap,
        IExposurePort& exposure,
        IGeminiFilterPort& geminiFilter,
        GeminiFilterConfig geminiConfig,
        Config config,
        IRiskPort* riskPort = nullptr);

    SignalEngine(
        IScannerPort& scanner,
        strategy::StrategyRegistry& registry,
        IAccountPort& account,
        IOrdersPort& orders,
        IExposurePort& exposure,
        IGeminiFilterPort& geminiFilter,
        GeminiFilterConfig geminiConfig,
        Config config,
        IRiskPort* riskPort = nullptr);

    SignalEngine(
        IScannerPort& scanner,
        strategy::StrategyRegistry& registry,
        IAccountPort& account,
        IOrdersPort& orders,
        IOrderCapPort& orderCap,
        IExposurePort& exposure,
        Config config,
        IRiskPort* riskPort = nullptr);

    SignalEngine(
        IScannerPort& scanner,
        strategy::StrategyRegistry& registry,
        IAccountPort& account,
        IOrdersPort& orders,
        IExposurePort& exposure,
        Config config,
        IRiskPort* riskPort = nullptr);

    boost::asio::awaitable<void> run();
    void stop();

    boost::asio::awaitable<void> runScanCycle();
    boost::asio::awaitable<void> processItem(const WorkItem& item);
    boost::asio::awaitable<Result<void>> openPosition(
        std::string_view symbol,
        std::string_view signalInterval,
        strategy::Signal::Direction direction,
        double atr,
        double currentPrice,
        const strategy::StrategyConfig& cfg,
        std::string_view signalReason);
    boost::asio::awaitable<void> monitorTimeExit();
    boost::asio::awaitable<void> monitorTrailingStops();
    boost::asio::awaitable<void> reconcileTrackedPositions();
    boost::asio::awaitable<void> reconcileTrackedPositions(const account::AccountSnapshot& snapshot);
    boost::asio::awaitable<void> processExpiredPositions(std::chrono::system_clock::time_point now);
    boost::asio::awaitable<void> processTrailingStops();
    boost::asio::awaitable<void> logExposureMetrics();
    boost::asio::awaitable<void> notifyRiskPositionClosed();

    void onUserDataEvent(const UserDataEvent& event);
    const PositionTracker& tracker() const { return m_tracker; }
    PositionTracker& tracker() { return m_tracker; }

    using ScanCycleStatusCb = std::function<void(int queueItems, int openPositions)>;
    void setScanCycleStatusCallback(ScanCycleStatusCb cb);

private:
    struct GeminiCycleGate {
        bool closed{false};
        std::string reason;
        std::string firstSymbol;
        std::string firstTf;
        int skippedItems{0};
    };

    bool shouldSkipForClosedGeminiGate() const;
    void closeGeminiGateForCycle(std::string reason, std::string_view symbol, std::string_view tf);
    boost::asio::awaitable<GeminiFilterResult> evaluateGeminiNonBlocking(
        std::string symbol,
        strategy::Signal::Direction direction,
        std::string signalInterval);
    boost::asio::awaitable<std::optional<double>> livePositionQuantity(std::string_view symbol);
    static bool isFlatPositionQty(double quantity);
    static std::string accountErrorToString(const account::AccountServiceError& error);

    IScannerPort& m_scanner;
    strategy::StrategyRegistry& m_registry;
    IAccountPort& m_account;
    IOrdersPort& m_orders;
    IOrderCapPort& m_orderCap;
    IExposurePort& m_exposure;
    IGeminiFilterPort& m_geminiFilter;
    IRiskPort* m_riskPort{nullptr};
    GeminiFilterConfig m_geminiConfig;
    int m_geminiEvaluationsThisCycle{0};
    GeminiCycleGate m_geminiCycleGate;
    Config m_config;
    PositionTracker m_tracker;
    std::unique_ptr<LossManager> m_lossManager;
    TrailingStopController m_trailingStops;
    std::atomic<bool> m_running{false};
    ScanCycleStatusCb m_scanCycleStatusCb;
    boost::asio::steady_timer m_scanSleepTimer;
    boost::asio::steady_timer m_timeExitTimer;
    boost::asio::steady_timer m_trailingTimer;
};

class ScannerPort final : public IScannerPort {
public:
    explicit ScannerPort(scanner::MarketScanner& scanner) : m_scanner(scanner) {}
    const scanner::KlineCache& cache() const override { return m_scanner.cache(); }
    std::vector<std::string> symbols() const override { return m_scanner.symbols(); }
    std::optional<ExchangeSymbol> symbolInfo(std::string_view symbol) const override {
        return m_scanner.symbolInfo(symbol);
    }
    boost::asio::io_context& ioContext() override { return m_scanner.ioContext(); }

private:
    scanner::MarketScanner& m_scanner;
};

class AccountPort final : public IAccountPort {
public:
    explicit AccountPort(account::AccountService& account) : m_account(account) {}
    boost::asio::awaitable<account::AccountServiceResult<account::AccountSnapshot>> snapshot(
        account::AccountSnapshotRequest request = {}) override {
        co_return co_await m_account.snapshot(request);
    }

private:
    account::AccountService& m_account;
};

class OrdersPort final : public IOrdersPort {
public:
    explicit OrdersPort(Orders& orders) : m_orders(orders) {}
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> market(MarketOrderDraft draft) override {
        co_return co_await m_orders.market(std::move(draft));
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> limit(LimitOrderDraft draft) override {
        co_return co_await m_orders.limit(std::move(draft));
    }
    boost::asio::awaitable<OrdersResult<NormalOrderSnapshot>> amendLimitOrder(AmendLimitOrderDraft draft) override {
        co_return co_await m_orders.amendLimitOrder(std::move(draft));
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> protection(ProtectionOrderDraft draft) override {
        co_return co_await m_orders.protection(std::move(draft));
    }
    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> closeByMarket(CloseByMarketDraft draft) override {
        co_return co_await m_orders.closeByMarket(std::move(draft));
    }
    boost::asio::awaitable<OrdersResult<LeverageResult>> setLeverage(Symbol symbol, int leverage) override {
        co_return co_await m_orders.setLeverage(std::move(symbol), leverage);
    }
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelNormalByOrderId(Symbol symbol, int64_t orderId) override {
        co_return co_await m_orders.cancelNormalByOrderId(std::move(symbol), orderId);
    }
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelNormalByClientOrderId(
        Symbol symbol,
        ClientOrderId clientOrderId) override {
        co_return co_await m_orders.cancelNormalByClientOrderId(std::move(symbol), std::move(clientOrderId));
    }
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelAlgoByAlgoId(Symbol symbol, int64_t algoId) override {
        co_return co_await m_orders.cancelAlgoByAlgoId(std::move(symbol), algoId);
    }
    boost::asio::awaitable<OrdersResult<NormalCancelResult>> cancelAlgoByClientAlgoId(
        Symbol symbol,
        ClientAlgoId clientAlgoId) override {
        co_return co_await m_orders.cancelAlgoByClientAlgoId(std::move(symbol), std::move(clientAlgoId));
    }

private:
    Orders& m_orders;
};

} // namespace engine
