#include "engine/signal_engine.h"

#include "engine/sizing_policy.h"
#include "logger.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <iomanip>
#include <sstream>
#include <type_traits>

namespace engine {

namespace {

std::optional<DecimalString> toDecimal(double value) {
    std::ostringstream out;
    out << std::setprecision(16) << value;
    auto parsed = DecimalString::parse(out.str());
    if (!parsed) {
        return std::nullopt;
    }
    return *parsed;
}

std::string makeExitClientOrderId(std::string_view symbol, std::string_view suffix) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::string(symbol) + "_" + std::string(suffix) + "_" + std::to_string(now);
}

std::string fmt2(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

OrderSide openSide(strategy::Signal::Direction direction) {
    return direction == strategy::Signal::Direction::Long ? OrderSide::Buy : OrderSide::Sell;
}

OrderSide closeSide(strategy::Signal::Direction direction) {
    return direction == strategy::Signal::Direction::Long ? OrderSide::Sell : OrderSide::Buy;
}

} // namespace

SignalEngine::SignalEngine(
    IScannerPort& scanner,
    strategy::StrategyRegistry& registry,
    IAccountPort& account,
    IOrdersPort& orders,
    IExposurePort& exposure,
    Config config)
    : m_scanner(scanner),
      m_registry(registry),
      m_account(account),
      m_orders(orders),
      m_exposure(exposure),
      m_config(config) {}

boost::asio::awaitable<void> SignalEngine::run() {
    if (m_running.exchange(true)) {
        co_return;
    }

    account::AccountSnapshotRequest request;
    request.includePositions = true;
    auto snapshot = co_await m_account.snapshot(request);
    if (snapshot && snapshot->positions.has_value()) {
        m_tracker.loadFromSnapshot(*snapshot->positions);
    }

    boost::asio::co_spawn(
        m_scanner.ioContext(),
        [this] { return monitorTimeExit(); },
        boost::asio::detached);

    if (m_config.monitorTrailingStops) {
        boost::asio::co_spawn(
            m_scanner.ioContext(),
            [this] { return monitorTrailingStops(); },
            boost::asio::detached);
    }

    while (m_running) {
        co_await runScanCycle();
    }
}

void SignalEngine::stop() {
    m_running = false;
}

boost::asio::awaitable<void> SignalEngine::runScanCycle() {
    const auto symbols = m_scanner.symbols();
    const auto queue = WorkQueue::build(symbols, m_registry);

    for (const auto& item : queue) {
        if (!m_running) {
            co_return;
        }
        co_await processItem(item);
    }

    co_await logExposureMetrics();

    if (m_scanCycleStatusCb) {
        m_scanCycleStatusCb(static_cast<int>(queue.size()), static_cast<int>(m_tracker.all().size()));
    }
    std::chrono::seconds minScanInterval{3600};
    const auto allStrategies = m_registry.all();
    if (allStrategies.empty()) {
        minScanInterval = std::chrono::seconds{60};
    } else {
        for (const auto* strategy : allStrategies) {
            minScanInterval = std::min(minScanInterval, strategy->config().scanInterval);
        }
    }

    boost::asio::steady_timer timer(m_scanner.ioContext());
    timer.expires_after(minScanInterval);
    boost::system::error_code ec;
    co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
    if (ec) {
        co_return;
    }
}

boost::asio::awaitable<void> SignalEngine::processItem(const WorkItem& item) {
    if (!item.strategy) {
        co_return;
    }
    if (m_tracker.has(item.symbol)) {
        co_return;
    }

    const auto klines = m_scanner.cache().snapshot(item.symbol, item.interval);
    if (!klines || klines->empty()) {
        co_return;
    }

    strategy::Signal signal;
    try {
        signal = item.strategy->evaluate(item.symbol, item.interval, *klines);
    } catch (const std::exception& e) {
        Logger::instance().log(
            LogLevel::Warning,
            "strategy evaluate exception strategy=" + item.strategy->config().name + " symbol=" + item.symbol +
                " reason=" + e.what());
        co_return;
    } catch (...) {
        Logger::instance().log(
            LogLevel::Warning,
            "strategy evaluate unknown exception strategy=" + item.strategy->config().name + " symbol=" + item.symbol);
        co_return;
    }

    if (signal.direction == strategy::Signal::Direction::None) {
        co_return;
    }
    const auto& cfg = item.strategy->config();
    if (signal.confidence < cfg.minConfidence) {
        co_return;
    }

    const double atr = signal.atr > 0.0 ? signal.atr : strategy::indicators::lastAtr(*klines, cfg.atrPeriod);
    if (atr <= 0.0) {
        co_return;
    }
    const double currentPrice = klines->back().close;
    if (currentPrice <= 0.0) {
        co_return;
    }

    (void)co_await openPosition(item.symbol, signal.direction, atr, currentPrice, cfg);
}

boost::asio::awaitable<Result<void>> SignalEngine::openPosition(
    std::string_view symbol,
    strategy::Signal::Direction direction,
    double atr,
    double currentPrice,
    const strategy::StrategyConfig& cfg) {
    const auto symbolMeta = m_scanner.symbolInfo(symbol);
    const double stepSize = symbolMeta.has_value() && symbolMeta->stepSize > 0.0 ? symbolMeta->stepSize : 0.001;
    account::AccountSnapshot snapshot;
    double balance = 0.0;
    account::AccountSnapshotRequest request;
    request.includePositions = true;
    const auto snapshotResult = co_await m_account.snapshot(request);
    if (snapshotResult) {
        snapshot = *snapshotResult;
        balance = snapshot.account.availableBalance;
    }

    auto size = calculateSize(
        SizingInput{
            .availableBalance = balance,
            .atr = atr,
            .riskPct = cfg.riskPct,
            .slMultiplier = cfg.slMultiplier,
            .minNotional = std::max(cfg.minNotional, m_config.minNotional),
        },
        currentPrice,
        stepSize);
    if (size.quantity <= 0.0) {
        co_return std::unexpected(BinanceError::fromApiResponse(-91000, "quantity is zero after sizing"));
    }

    ExposureCheckResult exposureResult;
    try {
        exposureResult = m_exposure.check(
            symbol,
            direction,
            size.notional,
            m_tracker,
            snapshot,
            balance);
    } catch (const std::exception& e) {
        Logger::instance().log(
            LogLevel::Error,
            "exposure check exception symbol=" + std::string(symbol) + " reason=" + e.what());
        if (m_exposure.failureMode() == ExposureFailureMode::Closed) {
            co_return Result<void>{};
        }
        exposureResult = {ExposureDecision::Allow, 1.0, "exposure check failed fail-open"};
    } catch (...) {
        Logger::instance().log(
            LogLevel::Error,
            "exposure check unknown exception symbol=" + std::string(symbol));
        if (m_exposure.failureMode() == ExposureFailureMode::Closed) {
            co_return Result<void>{};
        }
        exposureResult = {ExposureDecision::Allow, 1.0, "exposure check failed fail-open"};
    }

    if (exposureResult.decision == ExposureDecision::Block) {
        Logger::instance().log(
            LogLevel::Warning,
            "exposure blocked symbol=" + std::string(symbol) + " reason=" + exposureResult.reason);
        co_return Result<void>{};
    }

    if (exposureResult.decision == ExposureDecision::ScaleDown) {
        const double scaledNotional = size.notional * exposureResult.scaleFactor;
        if (scaledNotional < m_exposure.minNotionalAfterScale()) {
            Logger::instance().log(
                LogLevel::Warning,
                "exposure scaled too small symbol=" + std::string(symbol) + " reason=" + exposureResult.reason);
            co_return Result<void>{};
        }
        const double scaledRawQty = scaledNotional / currentPrice;
        const double scaledSteps = std::floor(scaledRawQty / stepSize);
        size.quantity = std::max(0.0, scaledSteps * stepSize);
        size.notional = size.quantity * currentPrice;
        if (size.quantity <= 0.0 || size.notional < m_exposure.minNotionalAfterScale()) {
            Logger::instance().log(
                LogLevel::Warning,
                "exposure scaled quantity invalid symbol=" + std::string(symbol) + " reason=" + exposureResult.reason);
            co_return Result<void>{};
        }
    }

    const auto qty = toDecimal(size.quantity);
    if (!qty) {
        co_return std::unexpected(BinanceError::fromParse("invalid quantity decimal"));
    }

    const auto side = openSide(direction);
    auto marketResult = co_await m_orders.market(MarketOrderDraft{
        .symbol = std::string(symbol),
        .side = side,
        .quantity = *qty,
        .positionSide = PositionSide::Both,
    });
    if (!marketResult) {
        co_return std::unexpected(marketResult.error());
    }
    if (marketResult->state != PlacementState::Accepted) {
        co_return std::unexpected(BinanceError::fromApiResponse(
            marketResult->binanceCode.value_or(-91001),
            marketResult->binanceMessage.value_or("market placement rejected")));
    }

    double entryPrice = currentPrice;
    if (marketResult->avgPrice.has_value()) {
        try {
            const double filledPrice = std::stod(*marketResult->avgPrice);
            if (filledPrice > 0.0) {
                entryPrice = filledPrice;
            }
        } catch (...) {
            Logger::instance().log(
                LogLevel::Warning,
                "market placement returned invalid avgPrice for symbol=" + std::string(symbol));
        }
    }
    const double tpPriceValue = direction == strategy::Signal::Direction::Long
        ? entryPrice + (atr * cfg.tpMultiplier)
        : entryPrice - (atr * cfg.tpMultiplier);
    const auto tpPrice = toDecimal(tpPriceValue);
    if (!tpPrice) {
        co_return std::unexpected(BinanceError::fromParse("invalid tp decimal"));
    }

    const auto close = closeSide(direction);
    const std::string tpClientOrderId = makeExitClientOrderId(symbol, "tp");
    std::string slClientOrderId;

    auto tpResult = co_await m_orders.limit(LimitOrderDraft{
        .symbol = std::string(symbol),
        .side = close,
        .quantity = *qty,
        .price = *tpPrice,
        .timeInForce = TimeInForce::GTC,
        .positionSide = PositionSide::Both,
        .reduceOnly = true,
        .clientOrderId = tpClientOrderId,
    });
    std::optional<NormalPlacementResult> slResult;
    double initialStopLevel = 0.0;
    if (m_config.placeStopLoss) {
        const double slPriceValue = direction == strategy::Signal::Direction::Long
            ? entryPrice - (atr * cfg.slMultiplier)
            : entryPrice + (atr * cfg.slMultiplier);
        initialStopLevel = slPriceValue;
        const auto slPrice = toDecimal(slPriceValue);
        if (!slPrice) {
            co_return std::unexpected(BinanceError::fromParse("invalid sl decimal"));
        }
        slClientOrderId = makeExitClientOrderId(symbol, "sl");
        auto protectionResult = co_await m_orders.protection(ProtectionOrderDraft{
            .symbol = std::string(symbol),
            .positionSide = PositionSide::Both,
            .closeSide = close,
            .kind = ProtectionKind::StopLoss,
            .triggerPrice = *slPrice,
            .closeQuantity = *qty,
            .clientAlgoId = slClientOrderId,
        });
        if (protectionResult) {
            slResult = *protectionResult;
        }
    }

    TrackedPosition tracked;
    tracked.symbol = std::string(symbol);
    tracked.direction = direction;
    tracked.openedAt = std::chrono::system_clock::now();
    tracked.maxHoldDuration = cfg.maxHoldDuration;
    tracked.entryPrice = entryPrice;
    tracked.quantity = size.quantity;
    tracked.tpClientOrderId = tpClientOrderId;
    tracked.slClientOrderId = slClientOrderId;
    tracked.trailingEnabled = cfg.trailingStop.enabled;
    tracked.trailingInterval = cfg.trailingStop.interval.empty()
        ? (cfg.intervals.empty() ? std::string{} : cfg.intervals.front())
        : cfg.trailingStop.interval;
    tracked.trailingCandles = cfg.trailingStop.candles;
    tracked.trailingCheckInterval = cfg.trailingStop.checkInterval;
    tracked.currentTrailLevel = initialStopLevel;
    if (tpResult && tpResult->orderId.has_value()) {
        tracked.tpOrderId = *tpResult->orderId;
    }
    if (slResult && slResult->orderId.has_value()) {
        tracked.slOrderId = *slResult->orderId;
    }
    m_tracker.add(std::move(tracked));
    co_return Result<void>{};
}

boost::asio::awaitable<void> SignalEngine::logExposureMetrics() {
    account::AccountSnapshotRequest request;
    request.includePositions = true;
    const auto snapshotResult = co_await m_account.snapshot(request);
    if (!snapshotResult) {
        const auto reason = std::visit(
            [](const auto& err) -> std::string {
                using T = std::decay_t<decltype(err)>;
                if constexpr (std::is_same_v<T, BinanceError>) {
                    return err.toString();
                }
                return "AccountMappingError";
            },
            snapshotResult.error());
        Logger::instance().log(
            LogLevel::Warning,
            "exposure metrics snapshot failed reason=" + reason);
        co_return;
    }

    const auto& snapshot = *snapshotResult;
    const double balance = snapshot.account.availableBalance;
    const auto metrics = m_exposure.currentMetrics(m_tracker, snapshot, balance);
    const double netPct = balance != 0.0 ? metrics.netBetaExposure / balance : 0.0;
    const double grossPct = balance != 0.0 ? metrics.grossBetaExposure / balance : 0.0;

    Logger::instance().log(
        LogLevel::Info,
        "exposure metrics"
        " positions=" + std::to_string(metrics.positionCount) +
            " balance=" + fmt2(balance) +
            " long_beta=" + fmt2(metrics.longBetaExposure) +
            " short_beta=" + fmt2(metrics.shortBetaExposure) +
            " net_beta=" + fmt2(metrics.netBetaExposure) +
            " gross_beta=" + fmt2(metrics.grossBetaExposure) +
            " net_x_balance=" + fmt2(netPct) +
            " gross_x_balance=" + fmt2(grossPct));
}

boost::asio::awaitable<void> SignalEngine::monitorTimeExit() {
    boost::asio::steady_timer timer(m_scanner.ioContext());
    while (m_running) {
        timer.expires_after(m_config.positionCheckInterval);
        boost::system::error_code ec;
        co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) {
            co_return;
        }
        if (!m_running) {
            co_return;
        }

        co_await processExpiredPositions(std::chrono::system_clock::now());
    }
}

boost::asio::awaitable<void> SignalEngine::monitorTrailingStops() {
    boost::asio::steady_timer timer(m_scanner.ioContext());
    while (m_running) {
        const auto interval = m_config.trailingCheckInterval.count() > 0
            ? m_config.trailingCheckInterval
            : std::chrono::seconds{300};
        auto wakeInterval = interval;
        for (const auto& pos : m_tracker.all()) {
            if (pos.trailingEnabled && pos.trailingCheckInterval.count() > 0) {
                wakeInterval = std::min(wakeInterval, pos.trailingCheckInterval);
            }
        }
        timer.expires_after(wakeInterval);
        boost::system::error_code ec;
        co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) {
            co_return;
        }
        if (!m_running) {
            co_return;
        }

        co_await processTrailingStops();
    }
}

boost::asio::awaitable<void> SignalEngine::processExpiredPositions(std::chrono::system_clock::time_point now) {
    const auto expired = m_tracker.expired(now);
    for (const auto& pos : expired) {
        if (pos.tpOrderId > 0) {
            (void)co_await m_orders.cancelNormalByOrderId(pos.symbol, pos.tpOrderId);
        } else if (!pos.tpClientOrderId.empty()) {
            (void)co_await m_orders.cancelNormalByClientOrderId(pos.symbol, pos.tpClientOrderId);
        }

        if (pos.slOrderId > 0) {
            (void)co_await m_orders.cancelAlgoByAlgoId(pos.symbol, pos.slOrderId);
        } else if (!pos.slClientOrderId.empty()) {
            (void)co_await m_orders.cancelAlgoByClientAlgoId(pos.symbol, pos.slClientOrderId);
        }

        const auto qty = toDecimal(pos.quantity);
        if (qty) {
            (void)co_await m_orders.closeByMarket(
                CloseByMarketDraft{pos.symbol, closeSide(pos.direction), *qty});
        }

        m_tracker.remove(pos.symbol);
    }
}

boost::asio::awaitable<void> SignalEngine::processTrailingStops() {
    const auto positions = m_tracker.all();
    for (const auto& pos : positions) {
        const auto decision = m_trailingStops.evaluate(pos, m_scanner.cache());
        if (!decision) {
            continue;
        }

        bool knownStop = false;
        bool cancelSucceeded = false;
        if (pos.slOrderId > 0) {
            knownStop = true;
            auto cancel = co_await m_orders.cancelAlgoByAlgoId(pos.symbol, pos.slOrderId);
            cancelSucceeded = cancel.has_value();
        } else if (!pos.slClientOrderId.empty()) {
            knownStop = true;
            auto cancel = co_await m_orders.cancelAlgoByClientAlgoId(pos.symbol, pos.slClientOrderId);
            cancelSucceeded = cancel.has_value();
        }

        if (knownStop && !cancelSucceeded) {
            Logger::instance().log(
                LogLevel::Warning,
                "trailing stop cancel failed symbol=" + pos.symbol);
            continue;
        }
        if (knownStop) {
            m_tracker.updateStopLoss(pos.symbol, 0, {}, 0.0);
        }
        if (!knownStop && pos.currentTrailLevel > 0.0) {
            Logger::instance().log(
                LogLevel::Warning,
                "trailing stop has no known stop order id symbol=" + pos.symbol);
            continue;
        }

        const auto qty = toDecimal(pos.quantity);
        const auto trigger = toDecimal(decision->newLevel);
        if (!qty || !trigger) {
            Logger::instance().log(
                LogLevel::Warning,
                "trailing stop decimal conversion failed symbol=" + pos.symbol);
            continue;
        }

        const auto clientOrderId = makeExitClientOrderId(pos.symbol, "sltrail");
        auto protection = co_await m_orders.protection(ProtectionOrderDraft{
            .symbol = pos.symbol,
            .positionSide = PositionSide::Both,
            .closeSide = closeSide(pos.direction),
            .kind = ProtectionKind::StopLoss,
            .triggerPrice = *trigger,
            .closeQuantity = *qty,
            .clientAlgoId = clientOrderId,
        });
        if (!protection || protection->state != PlacementState::Accepted) {
            Logger::instance().log(
                LogLevel::Warning,
                "trailing stop placement failed symbol=" + pos.symbol);
            continue;
        }

        m_tracker.updateStopLoss(
            pos.symbol,
            protection->orderId.value_or(0),
            clientOrderId,
            decision->newLevel);
        Logger::instance().log(
            LogLevel::Info,
            "trailing stop updated symbol=" + pos.symbol + " reason=" + decision->reason);
    }
}

void SignalEngine::onUserDataEvent(const UserDataEvent& event) {
    const auto* order = std::get_if<OrderUpdateEvent>(&event);
    if (!order) {
        return;
    }
    if (order->orderStatus != "FILLED") {
        return;
    }
    if (!order->clientOrderId.empty()) {
        if (m_tracker.removeByExitOrderClientId(order->clientOrderId)) {
            return;
        }
    }
    if (!order->originalClientOrderId.empty()) {
        (void)m_tracker.removeByExitOrderClientId(order->originalClientOrderId);
    }
}

void SignalEngine::setScanCycleStatusCallback(ScanCycleStatusCb cb) {
    m_scanCycleStatusCb = std::move(cb);
}

} // namespace engine
