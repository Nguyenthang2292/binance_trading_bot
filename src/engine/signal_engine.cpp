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
#include <iomanip>
#include <sstream>

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
    Config config)
    : m_scanner(scanner), m_registry(registry), m_account(account), m_orders(orders), m_config(config) {}

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
    const double balance = co_await fetchAvailableBalance();

    const auto size = calculateSize(
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
    const double slPriceValue = direction == strategy::Signal::Direction::Long
        ? entryPrice - (atr * cfg.slMultiplier)
        : entryPrice + (atr * cfg.slMultiplier);

    const auto tpPrice = toDecimal(tpPriceValue);
    const auto slPrice = toDecimal(slPriceValue);
    if (!tpPrice || !slPrice) {
        co_return std::unexpected(BinanceError::fromParse("invalid tp/sl decimal"));
    }

    const auto close = closeSide(direction);
    const std::string tpClientOrderId = makeExitClientOrderId(symbol, "tp");
    const std::string slClientOrderId = makeExitClientOrderId(symbol, "sl");

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
    auto slResult = co_await m_orders.protection(ProtectionOrderDraft{
        .symbol = std::string(symbol),
        .positionSide = PositionSide::Both,
        .closeSide = close,
        .kind = ProtectionKind::StopLoss,
        .triggerPrice = *slPrice,
        .closeQuantity = *qty,
        .clientAlgoId = slClientOrderId,
    });

    TrackedPosition tracked;
    tracked.symbol = std::string(symbol);
    tracked.direction = direction;
    tracked.openedAt = std::chrono::system_clock::now();
    tracked.maxHoldDuration = cfg.maxHoldDuration;
    tracked.entryPrice = entryPrice;
    tracked.quantity = size.quantity;
    tracked.tpClientOrderId = tpClientOrderId;
    tracked.slClientOrderId = slClientOrderId;
    if (tpResult && tpResult->orderId.has_value()) {
        tracked.tpOrderId = *tpResult->orderId;
    }
    if (slResult && slResult->orderId.has_value()) {
        tracked.slOrderId = *slResult->orderId;
    }
    m_tracker.add(std::move(tracked));
    co_return Result<void>{};
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

boost::asio::awaitable<double> SignalEngine::fetchAvailableBalance() {
    const auto snapshot = co_await m_account.snapshot();
    if (!snapshot) {
        co_return 0.0;
    }
    co_return snapshot->account.availableBalance;
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
