#include "engine/signal_engine.h"

#include "engine/price_filter.h"
#include "engine/sizing_policy.h"
#include "logger.h"

#include <boost/asio/async_result.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <type_traits>
#include <unordered_map>

namespace engine {

namespace {

struct GeminiEvalOutcome {
    std::exception_ptr exception;
    GeminiFilterResult result;
};

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

std::string directionToString(strategy::Signal::Direction direction) {
    switch (direction) {
        case strategy::Signal::Direction::Long:
            return "Long";
        case strategy::Signal::Direction::Short:
            return "Short";
        case strategy::Signal::Direction::None:
            return "None";
    }
    return "None";
}

std::string executionModeToString(orchestration::ExecutionMode mode) {
    switch (mode) {
        case orchestration::ExecutionMode::Disabled:
            return "disabled";
        case orchestration::ExecutionMode::Shadow:
            return "shadow";
        case orchestration::ExecutionMode::ShadowOnly:
            return "shadow_only";
        case orchestration::ExecutionMode::LiveCanary:
            return "live_canary";
        case orchestration::ExecutionMode::Live:
            return "live";
    }
    return "shadow";
}

std::string geminiDecisionToString(GeminiDecision decision) {
    return decision == GeminiDecision::Allow ? "Allow" : "Block";
}

bool shouldBlockGeminiError(const GeminiFilterConfig& config, const GeminiFilterResult& result) {
    return config.blockOnError && (result.hasError || !result.errorCode.empty());
}

bool isQuotaExhaustedGeminiError(const GeminiFilterResult& result) {
    if (result.errorCode == "quota_exhausted") {
        return true;
    }
    std::string lowered = result.reason;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered.find("resource_exhausted") != std::string::npos ||
        lowered.find("rate limit") != std::string::npos ||
        lowered.find("quota") != std::string::npos;
}

NoOpGeminiFilterPort& defaultGeminiPort() {
    static NoOpGeminiFilterPort port;
    return port;
}

NoOpOrderCapPort& defaultOrderCapPort() {
    static NoOpOrderCapPort port;
    return port;
}

std::string fmt6(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    return out.str();
}

std::string quoteString(std::string_view value) {
    std::ostringstream out;
    out << std::quoted(std::string(value));
    return out.str();
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<OrderMetadata> buildOrderMetadata(
    std::string_view strategyName,
    std::string_view signalInterval,
    std::string_view signalReason) {
    OrderMetadata metadata;
    if (!strategyName.empty()) {
        metadata.strategyTag = std::string(strategyName);
    }
    if (!signalInterval.empty()) {
        metadata.timeframe = std::string(signalInterval);
    }
    if (!signalInterval.empty() || !signalReason.empty()) {
        std::string comment;
        if (!signalInterval.empty()) {
            comment = "tf=" + std::string(signalInterval);
        }
        if (!signalReason.empty()) {
            if (!comment.empty()) {
                comment += " ";
            }
            comment += "reason=" + std::string(signalReason);
        }
        metadata.comment = std::move(comment);
    }
    if (!metadata.strategyTag.has_value() && !metadata.timeframe.has_value() && !metadata.comment.has_value()) {
        return std::nullopt;
    }
    return metadata;
}

int clampOrderLeverage(int leverage) {
    constexpr int kMinLeverage = 1;
    constexpr int kMaxLeverage = 125;
    return std::clamp(leverage, kMinLeverage, kMaxLeverage);
}

std::pair<int, int> randomOrderLeverageRange(const SignalEngine::Config& engineConfig) {
    int minLeverage = clampOrderLeverage(engineConfig.randomLeverageMin);
    int maxLeverage = clampOrderLeverage(engineConfig.randomLeverageMax);
    if (minLeverage > maxLeverage) {
        std::swap(minLeverage, maxLeverage);
    }
    return {minLeverage, maxLeverage};
}

int configuredOrderLeverage(const strategy::StrategyConfig& cfg) {
    return clampOrderLeverage(cfg.leverage > 0 ? cfg.leverage : strategy::StrategyConfig{}.leverage);
}

int requestedOrderLeverage(const strategy::StrategyConfig& cfg, const SignalEngine::Config& engineConfig) {
    if (!engineConfig.randomLeverageEnabled) {
        return configuredOrderLeverage(cfg);
    }
    const auto [minLeverage, maxLeverage] = randomOrderLeverageRange(engineConfig);
    thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> distribution(minLeverage, maxLeverage);
    return distribution(rng);
}

double takeProfitDistance(double entryPrice, double atr, const strategy::StrategyConfig& cfg, int leverage) {
    if (cfg.takeProfitPercent > 0.0) {
        const double effectiveLeverage = leverage > 0 ? static_cast<double>(leverage) : 1.0;
        return entryPrice * cfg.takeProfitPercent / (100.0 * effectiveLeverage);
    }
    return atr * cfg.tpMultiplier;
}

std::chrono::seconds maxHoldDurationForInterval(
    const strategy::StrategyConfig& cfg,
    std::string_view signalInterval) {
    const auto it = cfg.maxHoldDurationByInterval.find(std::string(signalInterval));
    if (it != cfg.maxHoldDurationByInterval.end()) {
        return it->second;
    }
    return cfg.maxHoldDuration;
}

std::optional<int64_t> latestClosedCandleOpenTime(
    const scanner::KlineCache& cache,
    std::string_view symbol,
    std::string_view interval) {
    const auto klines = cache.snapshot(symbol, interval);
    if (!klines) {
        return std::nullopt;
    }
    for (auto it = klines->rbegin(); it != klines->rend(); ++it) {
        if (it->isClosed) {
            return it->openTime;
        }
    }
    return std::nullopt;
}

} // namespace

bool SignalEngine::shouldSkipForClosedGeminiGate() const {
    return m_geminiConfig.enabled &&
        m_geminiConfig.mode == GeminiFilterMode::Enforce &&
        m_geminiCycleGate.closed;
}

void SignalEngine::closeGeminiGateForCycle(std::string reason, std::string_view symbol, std::string_view tf) {
    if (m_geminiCycleGate.closed) {
        return;
    }
    m_geminiCycleGate.closed = true;
    m_geminiCycleGate.reason = std::move(reason);
    m_geminiCycleGate.firstSymbol = std::string(symbol);
    m_geminiCycleGate.firstTf = std::string(tf);
    Logger::instance().log(
        LogLevel::Warning,
        "gemini gate closed for cycle reason=" + quoteString(m_geminiCycleGate.reason) +
            " first_symbol=" + m_geminiCycleGate.firstSymbol +
            " first_tf=" + m_geminiCycleGate.firstTf +
            " policy=fail_closed");
}

std::string SignalEngine::accountErrorToString(const account::AccountServiceError& error) {
    return std::visit(
        [](const auto& err) -> std::string {
            using T = std::decay_t<decltype(err)>;
            if constexpr (std::is_same_v<T, BinanceError>) {
                return err.toString();
            } else {
                return "mapping_error=" + std::to_string(static_cast<int>(err));
            }
        },
        error);
}

bool SignalEngine::isFlatPositionQty(double quantity) {
    return std::abs(quantity) <= 1e-10;
}

boost::asio::awaitable<std::optional<double>> SignalEngine::livePositionQuantity(std::string_view symbol) {
    account::AccountSnapshotRequest request;
    request.includePositions = true;
    request.positionFilter = std::string(symbol);
    const auto snapshotResult = co_await m_account.snapshot(request);
    if (!snapshotResult) {
        Logger::instance().log(
            LogLevel::Warning,
            "position snapshot failed symbol=" + std::string(symbol) +
                " reason=" + quoteString(accountErrorToString(snapshotResult.error())));
        co_return std::nullopt;
    }

    double netQty = 0.0;
    if (snapshotResult->positions.has_value()) {
        for (const auto& position : *snapshotResult->positions) {
            if (position.symbol == symbol) {
                netQty += position.positionAmt;
            }
        }
        co_return std::abs(netQty);
    }

    for (const auto& position : snapshotResult->account.positions) {
        if (position.symbol == symbol) {
            netQty += position.positionAmt;
        }
    }
    co_return std::abs(netQty);
}

boost::asio::awaitable<GeminiFilterResult> SignalEngine::evaluateGeminiNonBlocking(
    std::string symbol,
    strategy::Signal::Direction direction,
    std::string signalInterval) {
    auto token = boost::asio::use_awaitable;
    auto outcome = co_await boost::asio::async_initiate<
        decltype(token),
        void(GeminiEvalOutcome)>(
        [this, symbol = std::move(symbol), direction, signalInterval = std::move(signalInterval)](
            auto completionHandler) mutable {
            auto completionExecutor = boost::asio::get_associated_executor(
                completionHandler,
                m_scanner.ioContext().get_executor());
            boost::asio::post(
                m_geminiEvaluationPool,
                [this,
                 symbol = std::move(symbol),
                 direction,
                 signalInterval = std::move(signalInterval),
                 completionExecutor,
                 completionHandler = std::move(completionHandler)]() mutable {
                    GeminiEvalOutcome outcome;
                    try {
                        outcome.result = m_geminiFilter.evaluate(symbol, direction, signalInterval, m_scanner.cache());
                    } catch (...) {
                        outcome.exception = std::current_exception();
                    }

                    boost::asio::post(
                        completionExecutor,
                        [outcome = std::move(outcome), completionHandler = std::move(completionHandler)]()
                            mutable {
                            completionHandler(std::move(outcome));
                        });
                });
        },
        token);

    if (outcome.exception) {
        std::rethrow_exception(outcome.exception);
    }
    co_return outcome.result;
}

SignalEngine::SignalEngine(
    IScannerPort& scanner,
    strategy::StrategyRegistry& registry,
    IAccountPort& account,
    IOrdersPort& orders,
    IOrderCapPort& orderCap,
    IExposurePort& exposure,
    IGeminiFilterPort& geminiFilter,
    GeminiFilterConfig geminiConfig,
    Config config,
    IRiskPort* riskPort)
    : m_scanner(scanner),
      m_registry(registry),
      m_account(account),
      m_orders(orders),
      m_orderCap(orderCap),
      m_exposure(exposure),
      m_geminiFilter(geminiFilter),
      m_riskPort(riskPort),
      m_geminiConfig(std::move(geminiConfig)),
      m_config(config),
      m_scanSleepTimer(m_scanner.ioContext()),
      m_timeExitTimer(m_scanner.ioContext()),
      m_trailingTimer(m_scanner.ioContext()) {
    if (m_config.lossManager.enabled) {
        m_lossManager = std::make_unique<LossManager>(
            m_config.lossManager,
            m_orders,
            m_orderCap,
            m_exposure,
            m_tracker,
            [this](std::string_view symbol) { return m_scanner.symbolInfo(symbol); });
    }
}

SignalEngine::SignalEngine(
    IScannerPort& scanner,
    strategy::StrategyRegistry& registry,
    IAccountPort& account,
    IOrdersPort& orders,
    IExposurePort& exposure,
    IGeminiFilterPort& geminiFilter,
    GeminiFilterConfig geminiConfig,
    Config config,
    IRiskPort* riskPort)
    : SignalEngine(
          scanner,
          registry,
          account,
          orders,
          defaultOrderCapPort(),
          exposure,
          geminiFilter,
          std::move(geminiConfig),
          config,
          riskPort) {}

SignalEngine::SignalEngine(
    IScannerPort& scanner,
    strategy::StrategyRegistry& registry,
    IAccountPort& account,
    IOrdersPort& orders,
    IOrderCapPort& orderCap,
    IExposurePort& exposure,
    Config config,
    IRiskPort* riskPort)
    : SignalEngine(
          scanner,
          registry,
          account,
          orders,
          orderCap,
          exposure,
          defaultGeminiPort(),
          GeminiFilterConfig{.enabled = false, .mode = GeminiFilterMode::Disabled},
          config,
          riskPort) {}

SignalEngine::SignalEngine(
    IScannerPort& scanner,
    strategy::StrategyRegistry& registry,
    IAccountPort& account,
    IOrdersPort& orders,
    IExposurePort& exposure,
    Config config,
    IRiskPort* riskPort)
    : SignalEngine(
          scanner,
          registry,
          account,
          orders,
          defaultOrderCapPort(),
          exposure,
          defaultGeminiPort(),
          GeminiFilterConfig{.enabled = false, .mode = GeminiFilterMode::Disabled},
          config,
          riskPort) {}

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
        [this](std::exception_ptr ep) {
            if (!ep) {
                return;
            }
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                Logger::instance().log(LogLevel::Error, std::string("monitorTimeExit exception: ") + e.what());
            } catch (...) {
                Logger::instance().log(LogLevel::Error, "monitorTimeExit unknown exception");
            }
            m_running = false;
        });

    if (m_config.monitorTrailingStops) {
        boost::asio::co_spawn(
            m_scanner.ioContext(),
            [this] { return monitorTrailingStops(); },
            [this](std::exception_ptr ep) {
                if (!ep) {
                    return;
                }
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    Logger::instance().log(
                        LogLevel::Error,
                        std::string("monitorTrailingStops exception: ") + e.what());
                } catch (...) {
                    Logger::instance().log(LogLevel::Error, "monitorTrailingStops unknown exception");
                }
                m_running = false;
            });
    }

    while (m_running) {
        co_await runScanCycle();
    }
}

void SignalEngine::stop() {
    m_running = false;
    boost::system::error_code ec;
    m_scanSleepTimer.cancel(ec);
    m_timeExitTimer.cancel(ec);
    m_trailingTimer.cancel(ec);
}

boost::asio::awaitable<void> SignalEngine::runScanCycle() {
    const auto symbols = m_scanner.symbols();
    const auto allStrategies = m_registry.all();
    const auto queue = WorkQueue::build(symbols, m_registry);
    m_geminiEvaluationsThisCycle = 0;
    m_geminiCycleGate = {};

    if (m_riskPort) {
        account::AccountSnapshotRequest request;
        request.includePositions = true;
        const auto snapshotResult = co_await m_account.snapshot(request);
        if (snapshotResult) {
            m_riskPort->onScanCycle(*snapshotResult, nowMs());
        } else {
            Logger::instance().log(
                LogLevel::Warning,
                "risk onScanCycle skipped: snapshot failed reason=" +
                    quoteString(accountErrorToString(snapshotResult.error())));
        }
    }

    std::unordered_map<std::string, DecisionArbiter> qlibArbiters;

    for (const auto& item : queue) {
        if (!m_running) {
            co_return;
        }
        if (shouldSkipForClosedGeminiGate()) {
            ++m_geminiCycleGate.skippedItems;
            continue;
        }

        if (item.strategy && item.strategy->config().type == "qlib_strategy_signal") {
            const auto klines = m_scanner.cache().snapshot(item.symbol, item.interval);
            if (!klines || klines->empty()) continue;

            try {
                auto signal = item.strategy->evaluate(item.symbol, item.interval, *klines);
                const auto& cfg = item.strategy->config();
                const std::string adapterId = qlibAdapterId(cfg);
                std::string key = item.symbol + "|" + item.interval;
                qlibArbiters[key].add(adapterId, cfg, signal);
            } catch (const std::exception& e) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "strategy evaluate exception strategy=" + item.strategy->config().name + " symbol=" + item.symbol +
                        " reason=" + e.what());
            } catch (...) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "strategy evaluate unknown exception strategy=" + item.strategy->config().name + " symbol=" + item.symbol);
            }
        } else {
            co_await processItem(item);
        }
    }

    for (auto& [key, arbiter] : qlibArbiters) {
        if (!m_running) co_return;
        auto pipePos = key.find('|');
        if (pipePos == std::string::npos) continue;
        std::string symbol = key.substr(0, pipePos);
        std::string interval = key.substr(pipePos + 1);
        co_await processQlibCandidates(symbol, interval, arbiter);
    }

    if (m_geminiCycleGate.closed) {
        Logger::instance().log(
            LogLevel::Info,
            "gemini gate cycle summary closed=true reason=" + quoteString(m_geminiCycleGate.reason) +
                " skipped_items=" + std::to_string(m_geminiCycleGate.skippedItems));
    }

    if (m_riskPort) {
        co_await m_riskPort->maybeRecompute(nowMs());
    }

    co_await logExposureMetrics();

    if (m_scanCycleStatusCb) {
        m_scanCycleStatusCb(static_cast<int>(queue.size()), static_cast<int>(m_tracker.all().size()));
    }
    std::chrono::seconds minScanInterval{3600};
    if (allStrategies.empty()) {
        minScanInterval = std::chrono::seconds{60};
    } else {
        for (const auto* strategy : allStrategies) {
            minScanInterval = std::min(minScanInterval, strategy->config().scanInterval);
        }
    }

    m_scanSleepTimer.expires_after(minScanInterval);
    boost::system::error_code ec;
    co_await m_scanSleepTimer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
    if (ec) {
        co_return;
    }
}

std::optional<SignalEngine::ArbiterCandidate> SignalEngine::DecisionArbiter::arbitrate(
    std::string_view symbol, 
    int maxPositions, 
    double maxRiskPct, 
    int currentPositions, 
    double currentRiskPct,
    std::vector<SignalEngine::ArbiterCandidate>& outRejected) 
{
    if (candidates.empty()) return std::nullopt;

    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.cfg.priority != b.cfg.priority) {
            return a.cfg.priority < b.cfg.priority;
        }
        if (a.signal.confidence != b.signal.confidence) {
            return a.signal.confidence > b.signal.confidence;
        }
        return a.strategyId < b.strategyId;
    });

    std::optional<ArbiterCandidate> selected;

    for (auto& cand : candidates) {
        if (cand.signal.shadowOnly) {
            if (cand.signal.reason.find("shadow_only") == std::string::npos) {
                cand.signal.reason += " (shadow_only)";
            }
            outRejected.push_back(cand);
            continue;
        }

        if (cand.signal.direction == strategy::Signal::Direction::None && cand.signal.reason.empty()) {
            // purely none, no reason, drop silently or reject depending on shadow requirements
            // let's record it as direction_none for shadow metrics
            cand.signal.reason = "direction_none";
            outRejected.push_back(cand);
            continue;
        }

        if (cand.signal.direction == strategy::Signal::Direction::None) {
            outRejected.push_back(cand);
            continue;
        }

        if (!selected) {
            // Check exposure caps for the candidate
            bool capped = false;
            if (maxPositions > 0 && currentPositions >= maxPositions) {
                cand.signal.reason += " (capped_by_aggregate_concurrent_positions)";
                capped = true;
            } else if (std::isfinite(maxRiskPct) && (currentRiskPct + cand.cfg.riskPct) > maxRiskPct) {
                cand.signal.reason += " (capped_by_aggregate_total_risk)";
                capped = true;
            } else if (cand.cfg.maxConcurrentPositions.has_value() && currentPositions >= cand.cfg.maxConcurrentPositions.value()) {
                cand.signal.reason += " (capped_by_concurrent_positions)";
                capped = true;
            } else if (cand.cfg.maxTotalRiskPct.has_value() && (currentRiskPct + cand.cfg.riskPct) > cand.cfg.maxTotalRiskPct.value()) {
                cand.signal.reason += " (capped_by_total_risk)";
                capped = true;
            }

            if (capped) {
                outRejected.push_back(cand);
            } else {
                selected = cand;
            }
        } else {
            cand.signal.reason += " (rejected_by_arbiter)";
            outRejected.push_back(cand);
        }
    }

    return selected;
}

boost::asio::awaitable<void> SignalEngine::processQlibCandidates(const std::string& symbol, const std::string& interval, DecisionArbiter& arbiter) {
    if (arbiter.candidates.empty()) co_return;

    const auto klines = m_scanner.cache().snapshot(symbol, interval);
    if (!klines || klines->empty()) co_return;

    const double currentPrice = klines->back().close;

    std::unordered_map<std::string, orchestration::RuntimeStateSnapshot> runtimeByAdapter;
    auto runtimeFor = [&](std::string_view adapterId) -> orchestration::RuntimeStateSnapshot {
        const std::string key(adapterId);
        if (const auto it = runtimeByAdapter.find(key); it != runtimeByAdapter.end()) {
            return it->second;
        }
        orchestration::RuntimeStateSnapshot state;
        if (m_executionStatePort) {
            state = m_executionStatePort->snapshotForAdapter(adapterId, interval);
        }
        if (!state.available) {
            state.mode = orchestration::ExecutionMode::Disabled;
            state.modelId = key;
            state.interval = interval;
        }
        runtimeByAdapter.emplace(key, state);
        return state;
    };

    auto isShadow = [](orchestration::ExecutionMode mode) {
        return mode == orchestration::ExecutionMode::Shadow ||
            mode == orchestration::ExecutionMode::ShadowOnly;
    };

    for (auto& cand : arbiter.candidates) {
        const auto state = runtimeFor(cand.strategyId);
        if (!state.available) {
            cand.signal.reason += cand.signal.reason.empty() ? "runtime_state_unavailable" : " (runtime_state_unavailable)";
            cand.signal.direction = strategy::Signal::Direction::None;
            cand.signal.wouldPlaceOrder = false;
            continue;
        }
        if (state.mode == orchestration::ExecutionMode::Disabled) {
            cand.signal.reason += cand.signal.reason.empty() ? "execution_mode=disabled" : " (execution_mode=disabled)";
            cand.signal.direction = strategy::Signal::Direction::None;
            cand.signal.wouldPlaceOrder = false;
            continue;
        }
        if (isShadow(state.mode)) {
            cand.signal.shadowOnly = true;
            cand.signal.wouldPlaceOrder = cand.signal.direction != strategy::Signal::Direction::None;
            if (cand.signal.reason.find("shadow_only") == std::string::npos) {
                cand.signal.reason += cand.signal.reason.empty() ? "shadow_only" : " (shadow_only)";
            }
        }
    }

    auto recordShadow = [&](const ArbiterCandidate& cand, const orchestration::RuntimeStateSnapshot& runtimeState, std::string blockedStage, bool wouldPlaceOrder, double atr, double currentPrice) {
        if (!m_shadowMetricsPort) return;
        if (!isShadow(runtimeState.mode) && !cand.signal.shadowOnly) return;

        orchestration::ShadowSignalRecord rec;
        rec.modelId = runtimeState.modelId.empty() ? cand.strategyId : runtimeState.modelId;
        rec.runId = runtimeState.activeRunId;
        rec.adapterId = cand.strategyId;
        rec.symbol = symbol;
        rec.interval = interval;
        rec.asofOpenTimeMs = (klines && !klines->empty()) ? klines->back().openTime : 0;
        rec.capturedAtMs = nowMs();
        rec.direction = cand.signal.direction;
        rec.confidence = cand.signal.confidence;
        rec.executionMode = runtimeState.mode;
        rec.blockedStage = std::move(blockedStage);
        rec.wouldPlaceOrder = wouldPlaceOrder;
        rec.currentPrice = currentPrice;
        rec.atr = atr;
        rec.reason = cand.signal.reason;
        
        try {
            m_shadowMetricsPort->recordShadowSignal(rec);
        } catch (const std::exception& e) {
            Logger::instance().log(LogLevel::Warning, "shadow metrics record failed symbol=" + symbol + " reason=" + e.what());
        } catch (...) {
            Logger::instance().log(LogLevel::Warning, "shadow metrics record failed with unknown error symbol=" + symbol);
        }
    };

    std::vector<ArbiterCandidate> rejected;

    const auto trackedPositions = m_tracker.all();
    int currentPositions = static_cast<int>(trackedPositions.size());
    double currentRiskPct = 0.0;
    for (const auto& pos : trackedPositions) {
        currentRiskPct += pos.riskPct;
    }
    int maxPos = m_config.qlibAggregateMaxConcurrentPositions.value_or(std::numeric_limits<int>::max());
    double maxRisk = m_config.qlibAggregateMaxTotalRiskPct.value_or(std::numeric_limits<double>::infinity());
    
    auto selectedOpt = arbiter.arbitrate(symbol, maxPos, maxRisk, currentPositions, currentRiskPct, rejected);

    if (selectedOpt) {
        for (const auto& rej : rejected) {
            if (rej.signal.shadowOnly || rej.signal.direction == strategy::Signal::Direction::None) {
                continue;
            }
            Logger::instance().log(
                LogLevel::Info,
                "[ARBITER][CONFLICT] symbol=" + symbol +
                    " interval=" + interval +
                    " winner=" + selectedOpt->strategyId +
                    " loser=" + rej.strategyId +
                    " winner_dir=" + directionToString(selectedOpt->signal.direction) +
                    " loser_dir=" + directionToString(rej.signal.direction) +
                    " reason=" + rej.signal.reason);
        }
    }

    for (const auto& rej : rejected) {
        const double atr = strategy::indicators::lastAtr(*klines, rej.cfg.atrPeriod);
        recordShadow(rej, runtimeFor(rej.strategyId), rej.signal.shadowOnly ? "shadow_only" : "arbitration", rej.signal.wouldPlaceOrder, atr, currentPrice);
    }

    if (!selectedOpt) {
        co_return;
    }

    auto& selected = *selectedOpt;

    const double atr = strategy::indicators::lastAtr(*klines, selected.cfg.atrPeriod);
    const auto selectedRuntime = runtimeFor(selected.strategyId);

    if (m_tracker.has(symbol)) {
        recordShadow(selected, selectedRuntime, "duplicate_position", false, atr, currentPrice);
        co_return;
    }

    const bool placeOrders =
        selectedRuntime.available &&
        (selectedRuntime.mode == orchestration::ExecutionMode::Live ||
         selectedRuntime.mode == orchestration::ExecutionMode::LiveCanary);
    double riskPctOverride = -1.0;
    if (selectedRuntime.mode == orchestration::ExecutionMode::LiveCanary) {
        const double mult = m_executionStatePort ? m_executionStatePort->canaryRiskMultiplier() : 1.0;
        riskPctOverride = selected.cfg.riskPct * std::clamp(mult, 0.0, 1.0);
    }

    auto res = co_await openPosition(
        symbol,
        interval,
        selected.signal.direction,
        atr,
        currentPrice,
        selected.cfg,
        selected.signal.reason,
        0.0,
        false,
        strategy::Signal::ExitPolicy::Default,
        0,
        riskPctOverride,
        placeOrders
    );
    (void)res;

    if (!placeOrders) {
        const std::string blocked = m_lastOpenDecision.blockedStage;
        recordShadow(selected, selectedRuntime, blocked, m_lastOpenDecision.wouldPlaceOrder, atr, currentPrice);
    }
}

boost::asio::awaitable<void> SignalEngine::processItem(const WorkItem& item) {
    if (shouldSkipForClosedGeminiGate()) {
        ++m_geminiCycleGate.skippedItems;
        co_return;
    }
    if (!item.strategy) {
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

    const auto& cfg = item.strategy->config();
    const bool isQlibStrategy = cfg.type == "qlib_model_signal";
    orchestration::RuntimeStateSnapshot runtimeState;
    if (isQlibStrategy && m_executionStatePort) {
        runtimeState = m_executionStatePort->snapshot();
        if (runtimeState.available && runtimeState.mode == orchestration::ExecutionMode::Disabled) {
            co_return;
        }
    }

    auto recordShadow = [&](std::string blockedStage, bool wouldPlaceOrder, double atr, double currentPrice) {
        if (!isQlibStrategy || !m_shadowMetricsPort) {
            return;
        }
        if (!runtimeState.available ||
            (runtimeState.mode != orchestration::ExecutionMode::Shadow &&
             runtimeState.mode != orchestration::ExecutionMode::ShadowOnly)) {
            return;
        }
        orchestration::ShadowSignalRecord rec;
        rec.modelId = runtimeState.modelId;
        rec.runId = runtimeState.activeRunId;
        rec.adapterId = qlibAdapterId(cfg);
        rec.symbol = item.symbol;
        rec.interval = item.interval;
        rec.asofOpenTimeMs = klines->empty() ? 0 : klines->back().openTime;
        rec.capturedAtMs = nowMs();
        rec.direction = signal.direction;
        rec.confidence = signal.confidence;
        rec.executionMode = runtimeState.mode;
        rec.blockedStage = std::move(blockedStage);
        rec.wouldPlaceOrder = wouldPlaceOrder;
        rec.currentPrice = currentPrice;
        rec.atr = atr;
        rec.reason = signal.reason;
        try {
            m_shadowMetricsPort->recordShadowSignal(rec);
        } catch (const std::exception& e) {
            Logger::instance().log(
                LogLevel::Warning,
                "shadow metrics record failed symbol=" + item.symbol + " reason=" + e.what());
        } catch (...) {
            Logger::instance().log(
                LogLevel::Warning,
                "shadow metrics record failed with unknown error symbol=" + item.symbol);
        }
    };

    if (signal.direction == strategy::Signal::Direction::None) {
        recordShadow("direction_none", false, 0.0, 0.0);
        co_return;
    }
    if (signal.confidence < cfg.minConfidence) {
        recordShadow("confidence", false, 0.0, 0.0);
        co_return;
    }

    const double atr = signal.atr > 0.0 ? signal.atr : strategy::indicators::lastAtr(*klines, cfg.atrPeriod);
    if (atr <= 0.0) {
        recordShadow("atr", false, atr, 0.0);
        co_return;
    }
    const double currentPrice = klines->back().close;
    if (currentPrice <= 0.0) {
        recordShadow("price", false, atr, currentPrice);
        co_return;
    }

    Logger::instance().log(
        LogLevel::Info,
        "strategy candidate signal strategy=" + quoteString(cfg.name) +
            " tf=" + item.interval +
            " symbol=" + item.symbol +
            " direction=" + directionToString(signal.direction) +
            " confidence=" + fmt2(signal.confidence) +
            " atr=" + fmt6(atr) +
            " reason=" + quoteString(signal.reason));

    if (m_tracker.has(item.symbol)) {
        Logger::instance().log(
            LogLevel::Info,
            "signal skipped strategy=" + quoteString(cfg.name) +
                " tf=" + item.interval +
                " symbol=" + item.symbol +
                " reason=" + quoteString("position already tracked"));
        recordShadow("tracked_position", false, atr, currentPrice);
        co_return;
    }

    bool placeOrders = true;
    double riskPctOverride = -1.0;
    if (isQlibStrategy && runtimeState.available) {
            if (runtimeState.mode == orchestration::ExecutionMode::Shadow ||
                runtimeState.mode == orchestration::ExecutionMode::ShadowOnly) {
                placeOrders = false;
            } else if (runtimeState.mode == orchestration::ExecutionMode::LiveCanary) {
            const double mult = m_executionStatePort ? m_executionStatePort->canaryRiskMultiplier() : 1.0;
            riskPctOverride = cfg.riskPct * std::clamp(mult, 0.0, 1.0);
        }
    }

    m_lastOpenDecision = {};
    (void)co_await openPosition(
        item.symbol,
        item.interval,
        signal.direction,
        atr,
        currentPrice,
        cfg,
        signal.reason,
        signal.initialStopPrice,
        signal.disableFixedTakeProfit,
        signal.exitPolicy,
        signal.swingLookback,
        riskPctOverride,
        placeOrders);

    if (!placeOrders) {
        const std::string blocked = m_lastOpenDecision.blockedStage;
        recordShadow(blocked, m_lastOpenDecision.wouldPlaceOrder, atr, currentPrice);
    }
}

boost::asio::awaitable<Result<void>> SignalEngine::openPosition(
    std::string_view symbol,
    std::string_view signalInterval,
    strategy::Signal::Direction direction,
    double atr,
    double currentPrice,
    const strategy::StrategyConfig& cfg,
    std::string_view signalReason,
    double initialStopPrice,
    bool disableFixedTakeProfit,
    strategy::Signal::ExitPolicy exitPolicy,
    int swingLookback,
    double riskPctOverride,
    bool placeOrders) {
    const auto symbolMeta = m_scanner.symbolInfo(symbol);
    m_lastOpenDecision = {};
    m_lastOpenDecision.blockedStage = "unknown";
    m_lastOpenDecision.wouldPlaceOrder = false;
    const double stepSize = symbolMeta.has_value() && symbolMeta->stepSize > 0.0 ? symbolMeta->stepSize : 0.001;
    const double tickSize = symbolMeta.has_value() ? symbolMeta->tickSize : 0.0;
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
            .riskPct = riskPctOverride >= 0.0 ? riskPctOverride : cfg.riskPct,
            .slMultiplier = cfg.slMultiplier,
            .minNotional = std::max(
                {cfg.minNotional,
                 m_config.minNotional,
                 symbolMeta.has_value() ? symbolMeta->minNotional : 0.0}),
            .maxNotional = m_config.maxPositionNotionalXAvailableBalance > 0.0
                ? balance * m_config.maxPositionNotionalXAvailableBalance
                : 0.0,
        },
        currentPrice,
        stepSize);
    if (size.quantity <= 0.0 && m_config.maxPositionNotionalXAvailableBalance > 0.0) {
        const double configuredMinNotional = std::max(
            {cfg.minNotional, m_config.minNotional, symbolMeta.has_value() ? symbolMeta->minNotional : 0.0});
        const double configuredMaxNotional = balance * m_config.maxPositionNotionalXAvailableBalance;
        if (configuredMaxNotional > 0.0 && configuredMaxNotional < configuredMinNotional) {
            Logger::instance().log(
                LogLevel::Warning,
                "sizing config inversion symbol=" + std::string(symbol) +
                    " min_notional=" + fmt2(configuredMinNotional) +
                    " max_notional=" + fmt2(configuredMaxNotional) +
                    " action=skip");
        }
    }
    if (size.quantity <= 0.0) {
        m_lastOpenDecision.blockedStage = "sizing";
        co_return std::unexpected(BinanceError::fromApiResponse(-91000, "quantity is zero after sizing"));
    }

    OrderCapResult orderCapResult;
    try {
        orderCapResult = m_orderCap.check(size.notional, snapshot, m_tracker);
    } catch (const std::exception& e) {
        Logger::instance().log(
            LogLevel::Error,
            "order cap check exception symbol=" + std::string(symbol) + " reason=" + e.what());
        if (m_orderCap.failureMode() == OrderCapFailureMode::Closed) {
            m_lastOpenDecision.blockedStage = "order_cap_exception";
            co_return Result<void>{};
        }
        orderCapResult = {OrderCapDecision::Allow, "order cap check failed fail-open"};
    } catch (...) {
        Logger::instance().log(
            LogLevel::Error,
            "order cap check unknown exception symbol=" + std::string(symbol));
        if (m_orderCap.failureMode() == OrderCapFailureMode::Closed) {
            m_lastOpenDecision.blockedStage = "order_cap_exception";
            co_return Result<void>{};
        }
        orderCapResult = {OrderCapDecision::Allow, "order cap check failed fail-open"};
    }

    if (orderCapResult.decision == OrderCapDecision::Block) {
        m_lastOpenDecision.blockedStage = "order_cap";
        Logger::instance().log(
            LogLevel::Warning,
            "order cap blocked symbol=" + std::string(symbol) + " reason=" + orderCapResult.reason);
        co_return Result<void>{};
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
            m_lastOpenDecision.blockedStage = "exposure_exception";
            co_return Result<void>{};
        }
        exposureResult = {ExposureDecision::Allow, 1.0, "exposure check failed fail-open"};
    } catch (...) {
        Logger::instance().log(
            LogLevel::Error,
            "exposure check unknown exception symbol=" + std::string(symbol));
        if (m_exposure.failureMode() == ExposureFailureMode::Closed) {
            m_lastOpenDecision.blockedStage = "exposure_exception";
            co_return Result<void>{};
        }
        exposureResult = {ExposureDecision::Allow, 1.0, "exposure check failed fail-open"};
    }

    if (exposureResult.decision == ExposureDecision::Block) {
        m_lastOpenDecision.blockedStage = "exposure";
        Logger::instance().log(
            LogLevel::Warning,
            "exposure blocked symbol=" + std::string(symbol) + " reason=" + exposureResult.reason);
        co_return Result<void>{};
    }

    if (exposureResult.decision == ExposureDecision::ScaleDown) {
        const double scaledNotional = size.notional * exposureResult.scaleFactor;
        if (scaledNotional < m_exposure.minNotionalAfterScale()) {
            m_lastOpenDecision.blockedStage = "exposure_scaled_too_small";
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
            m_lastOpenDecision.blockedStage = "exposure_scaled_invalid";
            Logger::instance().log(
                LogLevel::Warning,
                "exposure scaled quantity invalid symbol=" + std::string(symbol) + " reason=" + exposureResult.reason);
            co_return Result<void>{};
        }
    }

    if (m_riskPort && !m_riskPort->canOpenPosition()) {
        m_lastOpenDecision.blockedStage = "risk";
        Logger::instance().log(
            LogLevel::Warning,
            "risk gate blocked symbol=" + std::string(symbol) +
                " tf=" + std::string(signalInterval) +
                " status=HARD_BREACH");
        co_return Result<void>{};
    }

    if (m_geminiConfig.enabled && m_geminiConfig.mode != GeminiFilterMode::Disabled) {
        if (m_geminiEvaluationsThisCycle >= m_geminiConfig.maxEvaluationsPerScanCycle) {
            if (m_geminiConfig.mode == GeminiFilterMode::Enforce || m_geminiConfig.blockOnBudgetExhausted) {
                if (m_geminiConfig.mode == GeminiFilterMode::Enforce && m_geminiConfig.closeGateOnBudgetExhausted) {
                    closeGeminiGateForCycle("budget_exhausted", symbol, signalInterval);
                }
                Logger::instance().log(
                    LogLevel::Warning,
                    "gemini budget exhausted symbol=" + std::string(symbol) + " tf=" + std::string(signalInterval) +
                        " decision=Block");
                m_lastOpenDecision.blockedStage = "gemini_budget";
                co_return Result<void>{};
            }
            Logger::instance().log(
                LogLevel::Info,
                "gemini budget exhausted symbol=" + std::string(symbol) + " tf=" + std::string(signalInterval) +
                    " policy=allow");
        } else {
            ++m_geminiEvaluationsThisCycle;
            Logger::instance().log(
                LogLevel::Info,
                "gemini evaluation requested symbol=" + std::string(symbol) +
                    " tf=" + std::string(signalInterval) +
                    " mode=enforce" +
                    " budget_used=" + std::to_string(m_geminiEvaluationsThisCycle) +
                    "/" + std::to_string(m_geminiConfig.maxEvaluationsPerScanCycle));
            GeminiFilterResult geminiResult;
            try {
                geminiResult = co_await evaluateGeminiNonBlocking(
                    std::string(symbol),
                    direction,
                    std::string(signalInterval));
            } catch (const std::exception& e) {
                geminiResult = {
                    .decision = GeminiDecision::Block,
                    .confidence = 0.0,
                    .sentimentScore = 0.0,
                    .visionScore = 0.0,
                    .reason = std::string("gemini evaluate exception: ") + e.what(),
                    .errorCode = "evaluate_exception",
                    .hasError = true,
                };
            } catch (...) {
                geminiResult = {
                    .decision = GeminiDecision::Block,
                    .confidence = 0.0,
                    .sentimentScore = 0.0,
                    .visionScore = 0.0,
                    .reason = "gemini evaluate unknown exception",
                    .errorCode = "evaluate_unknown_exception",
                    .hasError = true,
                };
            }

            const bool geminiHasError = geminiResult.hasError || !geminiResult.errorCode.empty();
            if (geminiHasError) {
                if (m_geminiConfig.mode == GeminiFilterMode::Enforce &&
                    m_geminiConfig.closeGateOnQuotaExhausted &&
                    isQuotaExhaustedGeminiError(geminiResult)) {
                    closeGeminiGateForCycle("quota_exhausted", symbol, signalInterval);
                }
                if (shouldBlockGeminiError(m_geminiConfig, geminiResult)) {
                    Logger::instance().log(
                        LogLevel::Warning,
                        "gemini error blocked symbol=" + std::string(symbol) +
                            " tf=" + std::string(signalInterval) +
                            " error_code=" + quoteString(geminiResult.errorCode) +
                            " reason=" + quoteString(geminiResult.reason));
                    m_lastOpenDecision.blockedStage = "gemini_error";
                    co_return Result<void>{};
                }
                Logger::instance().log(
                    LogLevel::Warning,
                    "gemini error allowed by policy symbol=" + std::string(symbol) +
                        " tf=" + std::string(signalInterval) +
                        " error_code=" + quoteString(geminiResult.errorCode) +
                        " reason=" + quoteString(geminiResult.reason));
            } else if (geminiResult.decision == GeminiDecision::Block) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "gemini blocked symbol=" + std::string(symbol) +
                        " tf=" + std::string(signalInterval) +
                        " confidence=" + fmt2(geminiResult.confidence) +
                        " reason=" + quoteString(geminiResult.reason));
                m_lastOpenDecision.blockedStage = "gemini";
                co_return Result<void>{};
            }
        }
    }

    const auto qty = quantityToStepDecimal(size.quantity, stepSize);
    if (!qty) {
        m_lastOpenDecision.blockedStage = "qty_decimal";
        co_return std::unexpected(BinanceError::fromParse("invalid quantity decimal"));
    }
    if (!placeOrders) {
        m_lastOpenDecision.blockedStage.clear();
        m_lastOpenDecision.wouldPlaceOrder = true;
        co_return Result<void>{};
    }

    if (!m_tracker.reserve(std::string(symbol))) {
        m_lastOpenDecision.blockedStage = "tracked_position";
        Logger::instance().log(
            LogLevel::Info,
            "signal skipped strategy=" + quoteString(cfg.name) +
                " tf=" + std::string(signalInterval) +
                " symbol=" + std::string(symbol) +
                " reason=" + quoteString("position already tracked"));
        co_return Result<void>{};
    }

    const int requestedLeverage = requestedOrderLeverage(cfg, m_config);
    auto leverageResult = co_await m_orders.setLeverage(std::string(symbol), requestedLeverage);
    if (!leverageResult) {
        m_lastOpenDecision.blockedStage = "set_leverage_error";
        m_tracker.remove(symbol);
        Logger::instance().log(
            LogLevel::Warning,
            "set leverage failed symbol=" + std::string(symbol) +
                " requested_leverage=" + std::to_string(requestedLeverage) +
                " error=" + quoteString(leverageResult.error().toString()));
        co_return std::unexpected(leverageResult.error());
    }
    const int activeLeverage = leverageResult->leverage > 0 ? leverageResult->leverage : requestedLeverage;

    Logger::instance().log(
        LogLevel::Info,
        "opening position strategy=" + quoteString(cfg.name) +
            " tf=" + std::string(signalInterval) +
            " symbol=" + std::string(symbol) +
            " side=" + (direction == strategy::Signal::Direction::Long ? "BUY" : "SELL") +
            " qty=" + std::string(qty->value()) +
            " notional=" + fmt2(size.notional) +
            " leverage=" + std::to_string(activeLeverage) +
            " leverage_policy=" + std::string(m_config.randomLeverageEnabled ? "random" : "configured"));

    const auto metadata = buildOrderMetadata(cfg.name, signalInterval, signalReason);
    const auto side = openSide(direction);
    MarketOrderDraft marketDraft{
        .symbol = std::string(symbol),
        .side = side,
        .quantity = *qty,
        .positionSide = PositionSide::Both,
        .metadata = metadata,
    };
    
    auto marketResult = m_executionPlanner 
        ? co_await m_executionPlanner->executeMarket(std::move(marketDraft))
        : co_await m_orders.market(std::move(marketDraft));
    if (!marketResult) {
        m_lastOpenDecision.blockedStage = "market_order_error";
        m_tracker.remove(symbol);
        co_return std::unexpected(marketResult.error());
    }
    if (marketResult->state != PlacementState::Accepted) {
        m_lastOpenDecision.blockedStage = "market_order_rejected";
        m_tracker.remove(symbol);
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
    const auto close = closeSide(direction);
    std::string tpClientOrderId;
    std::string slClientOrderId;
    std::optional<NormalPlacementResult> tpResult;

    const bool shouldPlaceFixedTakeProfit =
        !disableFixedTakeProfit && (cfg.takeProfitPercent > 0.0 || cfg.tpMultiplier > 0.0);
    if (shouldPlaceFixedTakeProfit) {
        const double tpDistance = takeProfitDistance(entryPrice, atr, cfg, activeLeverage);
        const double tpPriceValue = direction == strategy::Signal::Direction::Long
            ? entryPrice + tpDistance
            : entryPrice - tpDistance;
        const auto tpRounding = direction == strategy::Signal::Direction::Long
            ? PriceRounding::Down
            : PriceRounding::Up;
        const auto tpPrice = priceToTickDecimal(tpPriceValue, tickSize, tpRounding);
        if (!tpPrice) {
            m_lastOpenDecision.blockedStage = "tp_decimal";
            if (qty) {
                (void)co_await m_orders.closeByMarket(CloseByMarketDraft{
                    std::string(symbol),
                    close,
                    *qty,
                });
            }
            m_tracker.remove(symbol);
            co_return std::unexpected(BinanceError::fromParse("invalid tp decimal"));
        }

        tpClientOrderId = makeExitClientOrderId(symbol, "tp");
        auto placedTp = co_await m_orders.limit(LimitOrderDraft{
            .symbol = std::string(symbol),
            .side = close,
            .quantity = *qty,
            .price = *tpPrice,
            .timeInForce = TimeInForce::GTC,
            .positionSide = PositionSide::Both,
            .reduceOnly = true,
            .clientOrderId = tpClientOrderId,
            .metadata = metadata,
        });
        if (!placedTp || placedTp->state != PlacementState::Accepted) {
            if (placedTp) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "take-profit placement rejected symbol=" + std::string(symbol) +
                        " state=" + std::to_string(static_cast<int>(placedTp->state)) +
                        " code=" + std::to_string(placedTp->binanceCode.value_or(-1)) +
                        " message=" + quoteString(placedTp->binanceMessage.value_or("unknown")));
            } else {
                Logger::instance().log(
                    LogLevel::Warning,
                    "take-profit placement failed symbol=" + std::string(symbol) +
                        " error=" + quoteString(placedTp.error().toString()));
            }

            if (qty) {
                (void)co_await m_orders.closeByMarket(CloseByMarketDraft{
                    std::string(symbol),
                    close,
                    *qty,
                });
            }
            m_tracker.remove(symbol);
            if (placedTp) {
                m_lastOpenDecision.blockedStage = "tp_rejected";
                co_return std::unexpected(BinanceError::fromApiResponse(
                    placedTp->binanceCode.value_or(-91002),
                    placedTp->binanceMessage.value_or("take-profit placement rejected")));
            }
            m_lastOpenDecision.blockedStage = "tp_error";
            co_return std::unexpected(placedTp.error());
        }
        tpResult = *placedTp;
    }

    std::optional<NormalPlacementResult> slResult;
    double initialStopLevel = 0.0;
    if (initialStopPrice > 0.0) {
        const double stopTickOffset = tickSize > 0.0 ? tickSize : 0.0;
        initialStopLevel = direction == strategy::Signal::Direction::Long
            ? initialStopPrice - stopTickOffset
            : initialStopPrice + stopTickOffset;
    } else {
        initialStopLevel = direction == strategy::Signal::Direction::Long
            ? entryPrice - (atr * cfg.slMultiplier)
            : entryPrice + (atr * cfg.slMultiplier);
    }

    if (m_config.placeStopLoss || initialStopPrice > 0.0) {
        const bool customStop = initialStopPrice > 0.0;
        const auto slRounding = direction == strategy::Signal::Direction::Long
            ? (customStop ? PriceRounding::Down : PriceRounding::Up)
            : (customStop ? PriceRounding::Up : PriceRounding::Down);
        const auto slPrice = priceToTickDecimal(initialStopLevel, tickSize, slRounding);
        if (!slPrice) {
            m_lastOpenDecision.blockedStage = "sl_decimal";
            if (tpResult) {
                if (tpResult->orderId.has_value()) {
                    (void)co_await m_orders.cancelNormalByOrderId(std::string(symbol), *tpResult->orderId);
                } else if (!tpClientOrderId.empty()) {
                    (void)co_await m_orders.cancelNormalByClientOrderId(std::string(symbol), tpClientOrderId);
                }
            }
            if (qty) {
                (void)co_await m_orders.closeByMarket(CloseByMarketDraft{
                    std::string(symbol),
                    close,
                    *qty,
                });
            }
            m_tracker.remove(symbol);
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
            .metadata = metadata,
        });
        if (protectionResult && protectionResult->state == PlacementState::Accepted) {
            slResult = *protectionResult;
        } else {
            if (protectionResult) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "stop-loss placement rejected symbol=" + std::string(symbol) +
                        " state=" + std::to_string(static_cast<int>(protectionResult->state)) +
                        " code=" + std::to_string(protectionResult->binanceCode.value_or(-1)) +
                        " message=" + quoteString(protectionResult->binanceMessage.value_or("unknown")));
            } else {
                Logger::instance().log(
                    LogLevel::Warning,
                    "stop-loss placement failed symbol=" + std::string(symbol) +
                        " error=" + quoteString(protectionResult.error().toString()));
            }
            if (tpResult) {
                if (tpResult->orderId.has_value()) {
                    (void)co_await m_orders.cancelNormalByOrderId(std::string(symbol), *tpResult->orderId);
                } else if (!tpClientOrderId.empty()) {
                    (void)co_await m_orders.cancelNormalByClientOrderId(std::string(symbol), tpClientOrderId);
                }
            }
            if (qty) {
                (void)co_await m_orders.closeByMarket(CloseByMarketDraft{
                    std::string(symbol),
                    close,
                    *qty,
                });
            }
            m_tracker.remove(symbol);
            if (protectionResult) {
                m_lastOpenDecision.blockedStage = "sl_rejected";
                co_return std::unexpected(BinanceError::fromApiResponse(
                    protectionResult->binanceCode.value_or(-91003),
                    protectionResult->binanceMessage.value_or("stop-loss placement rejected")));
            }
            m_lastOpenDecision.blockedStage = "sl_error";
            co_return std::unexpected(protectionResult.error());
        }
    }

    TrackedPosition tracked;
    tracked.symbol = std::string(symbol);
    tracked.direction = direction;
    tracked.openedAt = std::chrono::system_clock::now();
    tracked.maxHoldDuration = maxHoldDurationForInterval(cfg, signalInterval);
    tracked.entryPrice = entryPrice;
    tracked.quantity = size.quantity;
    tracked.riskPct = cfg.riskPct;
    tracked.activeLeverage = activeLeverage;
    tracked.strategyName = cfg.name;
    tracked.signalInterval = std::string(signalInterval);
    tracked.signalReason = std::string(signalReason);
    tracked.tpClientOrderId = tpClientOrderId;
    tracked.slClientOrderId = slClientOrderId;
    tracked.trailingEnabled = cfg.trailingStop.enabled || exitPolicy == strategy::Signal::ExitPolicy::SwingTrailing;
    tracked.trailingInterval = signalInterval.empty()
        ? (cfg.trailingStop.interval.empty()
              ? (cfg.intervals.empty() ? std::string{} : cfg.intervals.front())
              : cfg.trailingStop.interval)
        : std::string(signalInterval);
    tracked.trailingCandles = cfg.trailingStop.candles;
    tracked.trailingCheckInterval = cfg.trailingStop.checkInterval;
    tracked.currentTrailLevel = initialStopLevel;
    tracked.trailingPolicy = exitPolicy;
    tracked.swingLookback = std::max(0, swingLookback);
    if (tpResult && tpResult->orderId.has_value()) {
        tracked.tpOrderId = *tpResult->orderId;
    }
    if (slResult && slResult->orderId.has_value()) {
        tracked.slOrderId = *slResult->orderId;
    }
    if (!m_tracker.commitReserved(symbol, std::move(tracked))) {
        m_lastOpenDecision.blockedStage = "tracker_commit";
        Logger::instance().log(
            LogLevel::Warning,
            "tracked position commit failed symbol=" + std::string(symbol) +
                " reason=" + quoteString("tracker state changed during open"));
        if (qty) {
            (void)co_await m_orders.closeByMarket(CloseByMarketDraft{
                std::string(symbol),
                close,
                *qty,
            });
        }
        m_tracker.remove(symbol);
        co_return Result<void>{};
    }
    m_lastOpenDecision.blockedStage.clear();
    m_lastOpenDecision.wouldPlaceOrder = true;
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

boost::asio::awaitable<void> SignalEngine::notifyRiskPositionClosed() {
    if (!m_riskPort) {
        co_return;
    }

    account::AccountSnapshotRequest request;
    request.includePositions = true;
    const auto snapshotResult = co_await m_account.snapshot(request);
    if (!snapshotResult) {
        Logger::instance().log(
            LogLevel::Warning,
            "risk onPositionClosed skipped: snapshot failed reason=" +
                quoteString(accountErrorToString(snapshotResult.error())));
        co_return;
    }
    m_riskPort->onPositionClosed(*snapshotResult, nowMs());
}

boost::asio::awaitable<void> SignalEngine::monitorTimeExit() {
    while (m_running) {
        m_timeExitTimer.expires_after(m_config.positionCheckInterval);
        boost::system::error_code ec;
        co_await m_timeExitTimer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) {
            co_return;
        }
        if (!m_running) {
            co_return;
        }

        account::AccountSnapshotRequest request;
        request.includePositions = true;
        const auto snapshotResult = co_await m_account.snapshot(request);
        if (!snapshotResult) {
            Logger::instance().log(
                LogLevel::Warning,
                "monitor time-exit snapshot failed reason=" + quoteString(accountErrorToString(snapshotResult.error())));
            continue;
        }

        co_await reconcileTrackedPositions(*snapshotResult);
        if (m_lossManager && m_lossManager->enabled()) {
            co_await m_lossManager->evaluate(*snapshotResult, snapshotResult->account.availableBalance);
        }
        co_await processExpiredPositions(std::chrono::system_clock::now());
    }
}

boost::asio::awaitable<void> SignalEngine::reconcileTrackedPositions() {
    account::AccountSnapshotRequest request;
    request.includePositions = true;
    const auto snapshotResult = co_await m_account.snapshot(request);
    if (!snapshotResult) {
        Logger::instance().log(
            LogLevel::Warning,
            "tracker reconciliation snapshot failed reason=" + quoteString(accountErrorToString(snapshotResult.error())));
        co_return;
    }

    co_await reconcileTrackedPositions(*snapshotResult);
    co_return;
}

boost::asio::awaitable<void> SignalEngine::reconcileTrackedPositions(const account::AccountSnapshot& snapshot) {
    std::unordered_map<std::string, double> liveQtyBySymbol;
    auto collectLive = [&liveQtyBySymbol](const std::vector<Position>& positions) {
        for (const auto& position : positions) {
            liveQtyBySymbol[position.symbol] += std::abs(position.positionAmt);
        }
    };
    if (snapshot.positions.has_value()) {
        collectLive(*snapshot.positions);
    } else {
        collectLive(snapshot.account.positions);
    }

    for (const auto& tracked : m_tracker.all()) {
        const auto it = liveQtyBySymbol.find(tracked.symbol);
        const double liveQty = it != liveQtyBySymbol.end() ? it->second : 0.0;
        if (!isFlatPositionQty(liveQty)) {
            continue;
        }
        if (m_tracker.removeIfOpenedAt(tracked.symbol, tracked.openedAt)) {
            if (m_riskPort) {
                m_riskPort->onPositionClosed(snapshot, nowMs());
            }
            Logger::instance().log(
                LogLevel::Info,
                "tracker reconciliation removed stale symbol=" + tracked.symbol);
        }
    }
    co_return;
}

boost::asio::awaitable<void> SignalEngine::monitorTrailingStops() {
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
        m_trailingTimer.expires_after(wakeInterval);
        boost::system::error_code ec;
        co_await m_trailingTimer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
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
        const auto currentTracked = m_tracker.bySymbol(pos.symbol);
        if (!currentTracked.has_value() || currentTracked->openedAt != pos.openedAt) {
            continue;
        }

        const auto liveQty = co_await livePositionQuantity(pos.symbol);
        if (liveQty.has_value() && isFlatPositionQty(*liveQty)) {
            (void)m_tracker.removeIfOpenedAt(pos.symbol, pos.openedAt);
            co_await notifyRiskPositionClosed();
            continue;
        }

        const double closeQty = liveQty.has_value() && *liveQty > 0.0 ? *liveQty : pos.quantity;
        const auto qty = toDecimal(closeQty);
        bool closeSucceeded = false;
        if (qty) {
            auto closeResult = co_await m_orders.closeByMarket(
                CloseByMarketDraft{pos.symbol, closeSide(pos.direction), *qty});
            closeSucceeded = closeResult.has_value();
        }

        if (closeSucceeded || (liveQty.has_value() && isFlatPositionQty(*liveQty))) {
            bool tpCancelOk = true;
            if (pos.tpOrderId > 0) {
                auto cancelTp = co_await m_orders.cancelNormalByOrderId(pos.symbol, pos.tpOrderId);
                tpCancelOk = cancelTp.has_value();
            } else if (!pos.tpClientOrderId.empty()) {
                auto cancelTp = co_await m_orders.cancelNormalByClientOrderId(pos.symbol, pos.tpClientOrderId);
                tpCancelOk = cancelTp.has_value();
            }
            if (!tpCancelOk) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "time-exit TP cancel failed after close symbol=" + pos.symbol);
            }

            bool slCancelOk = true;
            if (pos.slOrderId > 0) {
                auto cancelSl = co_await m_orders.cancelAlgoByAlgoId(pos.symbol, pos.slOrderId);
                slCancelOk = cancelSl.has_value();
            } else if (!pos.slClientOrderId.empty()) {
                auto cancelSl = co_await m_orders.cancelAlgoByClientAlgoId(pos.symbol, pos.slClientOrderId);
                slCancelOk = cancelSl.has_value();
            }
            if (!slCancelOk) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "time-exit stop cancel failed after close symbol=" + pos.symbol);
            }

            (void)m_tracker.removeIfOpenedAt(pos.symbol, pos.openedAt);
            co_await notifyRiskPositionClosed();
        } else {
            Logger::instance().log(
                LogLevel::Warning,
                "time-exit close failed symbol=" + pos.symbol);
        }
    }
}

boost::asio::awaitable<void> SignalEngine::processTrailingStops() {
    const auto positions = m_tracker.all();
    for (const auto& pos : positions) {
        const auto latest = m_tracker.bySymbol(pos.symbol);
        if (!latest.has_value() || latest->openedAt != pos.openedAt) {
            continue;
        }

        const auto latestClosed = latestClosedCandleOpenTime(m_scanner.cache(), pos.symbol, pos.trailingInterval);
        if (!latestClosed.has_value()) {
            continue;
        }
        if (*latestClosed <= pos.lastTrailingEvalCandleMs) {
            continue;
        }

        const auto decision = m_trailingStops.evaluate(pos, m_scanner.cache());
        if (!decision) {
            (void)m_tracker.markTrailingEvaluated(pos.symbol, *latestClosed);
            continue;
        }

        bool knownStop = false;
        bool canCancelOldAfterReplacement = false;
        if (pos.slOrderId > 0) {
            knownStop = true;
            canCancelOldAfterReplacement = true;
        } else if (!pos.slClientOrderId.empty()) {
            knownStop = true;
            canCancelOldAfterReplacement = true;
        }
        if (!knownStop && pos.currentTrailLevel > 0.0) {
            Logger::instance().log(
                LogLevel::Warning,
                "trailing stop has no known stop order id symbol=" + pos.symbol);
            continue;
        }

        const auto symbolMeta = m_scanner.symbolInfo(pos.symbol);
        const double stepSize = symbolMeta.has_value() && symbolMeta->stepSize > 0.0 ? symbolMeta->stepSize : 0.0;
        const auto qty = quantityToStepDecimal(pos.quantity, stepSize);
        const double tickSize = symbolMeta.has_value() ? symbolMeta->tickSize : 0.0;
        const auto triggerRounding = pos.direction == strategy::Signal::Direction::Long
            ? PriceRounding::Up
            : PriceRounding::Down;
        const auto trigger = priceToTickDecimal(decision->newLevel, tickSize, triggerRounding);
        if (!qty || !trigger) {
            Logger::instance().log(
                LogLevel::Warning,
                "trailing stop decimal conversion failed symbol=" + pos.symbol);
            continue;
        }

        const auto stillTracked = m_tracker.bySymbol(pos.symbol);
        if (!stillTracked.has_value() || stillTracked->openedAt != pos.openedAt) {
            continue;
        }

        const auto clientOrderId = makeExitClientOrderId(pos.symbol, "sltrail");
        const auto metadata = buildOrderMetadata(pos.strategyName, pos.signalInterval, pos.signalReason);
        auto protection = co_await m_orders.protection(ProtectionOrderDraft{
            .symbol = pos.symbol,
            .positionSide = PositionSide::Both,
            .closeSide = closeSide(pos.direction),
            .kind = ProtectionKind::StopLoss,
            .triggerPrice = *trigger,
            .closeQuantity = *qty,
            .clientAlgoId = clientOrderId,
            .metadata = metadata,
        });
        if (!protection || protection->state != PlacementState::Accepted) {
            Logger::instance().log(
                LogLevel::Warning,
                "trailing stop placement failed symbol=" + pos.symbol);
            continue;
        }

        if (canCancelOldAfterReplacement) {
            bool cancelSucceeded = false;
            if (pos.slOrderId > 0) {
                auto cancel = co_await m_orders.cancelAlgoByAlgoId(pos.symbol, pos.slOrderId);
                cancelSucceeded = cancel.has_value();
            } else if (!pos.slClientOrderId.empty()) {
                auto cancel = co_await m_orders.cancelAlgoByClientAlgoId(pos.symbol, pos.slClientOrderId);
                cancelSucceeded = cancel.has_value();
            }

            if (!cancelSucceeded) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "trailing stop old-order cancel failed symbol=" + pos.symbol);
            }
        }

        const auto trackedAfterPlacement = m_tracker.bySymbol(pos.symbol);
        if (!trackedAfterPlacement.has_value() || trackedAfterPlacement->openedAt != pos.openedAt) {
            continue;
        }

        m_tracker.updateStopLoss(
            pos.symbol,
            protection->orderId.value_or(0),
            clientOrderId,
            decision->newLevel);
        (void)m_tracker.markTrailingEvaluated(pos.symbol, *latestClosed);
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

    const bool isPartial = order->orderStatus == "PARTIALLY_FILLED";
    const bool isFilled = order->orderStatus == "FILLED";
    if (!isPartial && !isFilled) {
        return;
    }

    if (!order->clientOrderId.empty()) {
        if (isPartial && order->lastFilledQty > 0.0 &&
            m_tracker.applyExitFillByClientId(order->clientOrderId, order->lastFilledQty)) {
            return;
        }
        if (isFilled && m_tracker.removeByExitOrderClientId(order->clientOrderId)) {
            return;
        }
    }
    if (!order->originalClientOrderId.empty()) {
        if (isPartial && order->lastFilledQty > 0.0 &&
            m_tracker.applyExitFillByClientId(order->originalClientOrderId, order->lastFilledQty)) {
            return;
        }
        if (isFilled) {
            (void)m_tracker.removeByExitOrderClientId(order->originalClientOrderId);
        }
    }
}

void SignalEngine::setScanCycleStatusCallback(ScanCycleStatusCb cb) {
    m_scanCycleStatusCb = std::move(cb);
}

} // namespace engine
