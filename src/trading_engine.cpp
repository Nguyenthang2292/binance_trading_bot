#include "trading_engine.h"
#include "logger.h"
#include <numeric>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace {

constexpr double kPositionEpsilon = 1e-12;

std::optional<Quantity> toQuantity(double value) {
    std::ostringstream out;
    out << std::setprecision(16) << value;
    auto parsed = Quantity::parse(out.str());
    if (!parsed) {
        return std::nullopt;
    }
    return *parsed;
}

}

TradingEngine::TradingEngine(BinanceAPI& api, const TradingConfig& config)
    : m_api(api), m_config(config)
{
}

TradingEngine::~TradingEngine() {
    stop();
}

void TradingEngine::start() {
    if (m_running) return;
    m_running = true;
    m_workerThread = std::thread(&TradingEngine::tradingLoop, this);
    Logger::instance().log(LogLevel::Info, "Trading engine started for " + m_config.symbol);
}

void TradingEngine::stop() {
    if (!m_running) return;
    m_running = false;
    m_stopCv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    Logger::instance().log(LogLevel::Info, "Trading engine stopped for " + m_config.symbol);
}

bool TradingEngine::isRunning() const {
    return m_running;
}

std::vector<Kline> TradingEngine::getHistoricalData(size_t count) {
    return m_api.getKlines(m_config.symbol, m_config.interval, static_cast<int>(count));
}

double TradingEngine::calculateSMA(const std::vector<double>& prices, int period) {
    if (prices.size() < static_cast<size_t>(period)) return 0.0;
    double sum = 0.0;
    for (size_t i = prices.size() - period; i < prices.size(); i++) {
        sum += prices[i];
    }
    return sum / period;
}

double TradingEngine::calculateEMA(const std::vector<double>& prices, int period) {
    if (prices.size() < static_cast<size_t>(period)) return 0.0;

    double multiplier = 2.0 / (period + 1.0);
    double ema = prices[0];

    for (size_t i = 1; i < prices.size(); i++) {
        ema = (prices[i] - ema) * multiplier + ema;
    }
    return ema;
}

double TradingEngine::calculateRSI(const std::vector<double>& closes, int period) {
    if (closes.size() < static_cast<size_t>(period + 1)) return 50.0;

    double avgGain = 0.0;
    double avgLoss = 0.0;

    for (size_t i = 1; i <= static_cast<size_t>(period); i++) {
        double diff = closes[i] - closes[i - 1];
        if (diff > 0) {
            avgGain += diff;
        } else {
            avgLoss -= diff;
        }
    }

    avgGain /= period;
    avgLoss /= period;

    if (avgLoss == 0.0) return 100.0;

    double rs = avgGain / avgLoss;
    double rsi = 100.0 - (100.0 / (1.0 + rs));

    return rsi;
}

Signal TradingEngine::analyzeMarket(const std::vector<Kline>& klines) {
    Signal signal;
    signal.action = Signal::Action::HOLD;

    if (klines.size() < static_cast<size_t>(std::max({m_config.smaLongPeriod, m_config.rsiPeriod}) + 1)) {
        signal.reason = "Insufficient data";
        signal.confidence = 0.0;
        return signal;
    }

    std::vector<double> closes;
    closes.reserve(klines.size());
    for (const auto& k : klines) {
        closes.push_back(k.close);
    }

    double rsi = calculateRSI(closes, m_config.rsiPeriod);
    double smaShort = calculateSMA(closes, m_config.smaShortPeriod);
    double smaLong = calculateSMA(closes, m_config.smaLongPeriod);
    double currentPrice = closes.back();

    double macdLine = calculateEMA(closes, 12);
    double signalLine = calculateEMA(closes, 26);

    Logger::instance().log(LogLevel::Debug,
        m_config.symbol + " - Price: " + std::to_string(currentPrice) +
        " RSI: " + std::to_string(rsi) +
        " SMA_S: " + std::to_string(smaShort) +
        " SMA_L: " + std::to_string(smaLong));

    bool rsiOversold = rsi < m_config.rsiOversold;
    bool rsiOverbought = rsi > m_config.rsiOverbought;
    bool smaGoldenCross = smaShort > smaLong;
    bool smaDeathCross = smaShort < smaLong;
    bool macdBullish = macdLine > signalLine;

    int buySignals = 0;
    int sellSignals = 0;

    if (rsiOversold) buySignals++;
    if (smaGoldenCross) buySignals++;
    if (macdBullish) buySignals++;

    if (rsiOverbought) sellSignals++;
    if (smaDeathCross) sellSignals++;
    if (!macdBullish) sellSignals++;

    if (buySignals >= 2 && buySignals > sellSignals) {
        signal.action = Signal::Action::BUY;
        signal.confidence = static_cast<double>(buySignals) / 3.0;
        signal.reason = "BUY signal: RSI=" + std::to_string(rsi) +
                       " SMA_Cross=" + (smaGoldenCross ? "Golden" : "None") +
                       " MACD=" + (macdBullish ? "Bullish" : "Bearish");
    } else if (sellSignals >= 2 && sellSignals > buySignals) {
        signal.action = Signal::Action::SELL;
        signal.confidence = static_cast<double>(sellSignals) / 3.0;
        signal.reason = "SELL signal: RSI=" + std::to_string(rsi) +
                       " SMA_Cross=" + (smaDeathCross ? "Death" : "None") +
                       " MACD=" + (macdBullish ? "Bullish" : "Bearish");
    } else {
        signal.reason = "HOLD: No clear signal";
        signal.confidence = 0.0;
    }

    return signal;
}

void TradingEngine::executeSignal(const Signal& signal) {
    if (signal.action == Signal::Action::HOLD) return;

    const auto positionQty = currentPositionQty(m_config.symbol);
    if (!positionQty.has_value()) {
        Logger::instance().log(
            LogLevel::Warning,
            "Position state unavailable for " + m_config.symbol + ", skipping trade");
        return;
    }

    if (signal.action == Signal::Action::BUY) {
        if (*positionQty > kPositionEpsilon) {
            Logger::instance().log(LogLevel::Info, "Long position already open for " + m_config.symbol + ", skipping BUY");
            return;
        }

        const bool closingShort = *positionQty < -kPositionEpsilon;
        const double orderQty = closingShort
            ? std::min(std::abs(*positionQty), m_config.tradeQuantity)
            : m_config.tradeQuantity;

        Logger::instance().log(LogLevel::Trade,
            "Executing BUY: " + m_config.symbol + " qty=" + std::to_string(orderQty) +
            " confidence=" + std::to_string(signal.confidence));

        auto quantity = toQuantity(orderQty);
        if (!quantity) {
            Logger::instance().log(LogLevel::Error, "Invalid trade quantity for BUY placement");
            return;
        }

        auto placement = closingShort
            ? m_api.closeByMarket(CloseByMarketDraft{m_config.symbol, OrderSide::Buy, *quantity})
            : m_api.marketOrder(MarketOrderDraft{
                  .symbol = m_config.symbol,
                  .side = OrderSide::Buy,
                  .quantity = *quantity,
                  .positionSide = PositionSide::Both,
              });
        if (!placement) {
            Logger::instance().log(LogLevel::Error,
                "BUY order failed: " + placement.error().toString());
            return;
        }

        Logger::instance().log(LogLevel::Trade,
            "BUY order: id=" + std::to_string(placement->orderId.value_or(0)) +
            " status=" + placement->orderStatus.value_or("N/A"));

        if (m_onTrade) {
            m_onTrade(*placement);
        }
    } else if (signal.action == Signal::Action::SELL) {
        if (*positionQty <= kPositionEpsilon) {
            Logger::instance().log(LogLevel::Info, "No long position open for " + m_config.symbol + ", skipping SELL");
            return;
        }

        const double orderQty = std::min(*positionQty, m_config.tradeQuantity);
        Logger::instance().log(LogLevel::Trade,
            "Executing SELL: " + m_config.symbol + " qty=" + std::to_string(orderQty));

        auto quantity = toQuantity(orderQty);
        if (!quantity) {
            Logger::instance().log(LogLevel::Error, "Invalid trade quantity for SELL placement");
            return;
        }

        auto placement = m_api.closeByMarket(CloseByMarketDraft{m_config.symbol, OrderSide::Sell, *quantity});
        if (!placement) {
            Logger::instance().log(LogLevel::Error,
                "SELL order failed: " + placement.error().toString());
            return;
        }

        Logger::instance().log(LogLevel::Trade,
            "SELL order: id=" + std::to_string(placement->orderId.value_or(0)) +
            " status=" + placement->orderStatus.value_or("N/A"));

        if (m_onTrade) {
            m_onTrade(*placement);
        }
    }
}

std::optional<double> TradingEngine::currentPositionQty(const std::string& symbol) {
    auto positions = m_api.getPositions(symbol);
    if (!positions.has_value()) {
        Logger::instance().log(LogLevel::Warning, "Failed to check futures position for " + symbol);
        return std::nullopt;
    }

    double netQty = 0.0;
    for (const auto& position : *positions) {
        if (position.symbol == symbol) {
            netQty += position.positionAmt;
        }
    }
    return netQty;
}

bool TradingEngine::waitForNextPoll() {
    std::unique_lock lock(m_stopMutex);
    return m_stopCv.wait_for(
        lock,
        std::chrono::milliseconds(m_config.pollIntervalMs),
        [this] { return !m_running.load(); });
}

void TradingEngine::tradingLoop() {
    Logger::instance().log(LogLevel::Info, "Trading loop started for " + m_config.symbol);

    while (m_running) {
        try {
            auto klines = m_api.getKlines(m_config.symbol, m_config.interval,
                                          std::max({m_config.smaLongPeriod, m_config.rsiPeriod}) + 50);

            if (klines.empty()) {
                Logger::instance().log(LogLevel::Warning, "No kline data received for " + m_config.symbol);
                if (waitForNextPoll()) {
                    break;
                }
                continue;
            }

            Signal signal = analyzeMarket(klines);
            Logger::instance().log(LogLevel::Info,
                m_config.symbol + " Signal: " + signal.reason +
                " Confidence: " + std::to_string(signal.confidence));

            if (m_onSignal) {
                m_onSignal(signal);
            }

            executeSignal(signal);

        } catch (const std::exception& e) {
            Logger::instance().log(LogLevel::Error,
                "Error in trading loop: " + std::string(e.what()));
        }

        if (waitForNextPoll()) {
            break;
        }
    }
}

void TradingEngine::setOnSignal(std::function<void(const Signal&)> callback) {
    m_onSignal = std::move(callback);
}

void TradingEngine::setOnTrade(std::function<void(const NormalPlacementResult&)> callback) {
    m_onTrade = std::move(callback);
}
