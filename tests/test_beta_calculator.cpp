#include <gtest/gtest.h>

#include "engine/beta_calculator.h"

namespace {

std::vector<double> closesFromReturns(double start, const std::vector<double>& returns) {
    std::vector<double> closes;
    closes.reserve(returns.size() + 1);
    closes.push_back(start);
    for (double r : returns) {
        closes.push_back(closes.back() * (1.0 + r));
    }
    return closes;
}

void seedDaily(scanner::KlineCache& cache, std::string_view symbol, const std::vector<double>& closes) {
    for (size_t i = 0; i < closes.size(); ++i) {
        Kline k;
        k.openTime = static_cast<int64_t>(1000 + i);
        k.close = closes[i];
        cache.update(symbol, "1d", k);
    }
}

} // namespace

TEST(BetaCalculatorTest, ReturnsApproxTwoWhenCoinMovesDoubleBtc) {
    scanner::KlineCache cache(64);
    const std::vector<double> btcReturns{0.01, -0.02, 0.03, 0.015, -0.01};
    std::vector<double> coinReturns;
    coinReturns.reserve(btcReturns.size());
    for (double r : btcReturns) {
        coinReturns.push_back(2.0 * r);
    }
    seedDaily(cache, "BTCUSDT", closesFromReturns(100.0, btcReturns));
    seedDaily(cache, "ETHUSDT", closesFromReturns(200.0, coinReturns));

    engine::BetaCalculator calc;
    const auto beta = calc.calculate("ETHUSDT", cache, 30);
    ASSERT_TRUE(beta.has_value());
    EXPECT_NEAR(*beta, 2.0, 1e-6);
}

TEST(BetaCalculatorTest, ReturnsApproxNegativeOneForInverseSeries) {
    scanner::KlineCache cache(64);
    const std::vector<double> btcReturns{0.01, -0.02, 0.03, -0.015, 0.01};
    std::vector<double> coinReturns;
    coinReturns.reserve(btcReturns.size());
    for (double r : btcReturns) {
        coinReturns.push_back(-r);
    }
    seedDaily(cache, "BTCUSDT", closesFromReturns(100.0, btcReturns));
    seedDaily(cache, "XRPUSDT", closesFromReturns(10.0, coinReturns));

    engine::BetaCalculator calc;
    const auto beta = calc.calculate("XRPUSDT", cache, 30);
    ASSERT_TRUE(beta.has_value());
    EXPECT_NEAR(*beta, -1.0, 1e-6);
}

TEST(BetaCalculatorTest, ReturnsNulloptWhenBtcVarianceIsZero) {
    scanner::KlineCache cache(64);
    seedDaily(cache, "BTCUSDT", {100.0, 100.0, 100.0, 100.0});
    seedDaily(cache, "BNBUSDT", {200.0, 210.0, 220.0, 230.0});

    engine::BetaCalculator calc;
    const auto beta = calc.calculate("BNBUSDT", cache, 30);
    EXPECT_FALSE(beta.has_value());
}

TEST(BetaCalculatorTest, ReturnsNulloptWhenInsufficientData) {
    scanner::KlineCache cache(64);
    seedDaily(cache, "BTCUSDT", {100.0});
    seedDaily(cache, "SOLUSDT", {50.0});

    engine::BetaCalculator calc;
    const auto beta = calc.calculate("SOLUSDT", cache, 30);
    EXPECT_FALSE(beta.has_value());
}

TEST(BetaCalculatorTest, AlignsReturnsAndSkipsCorruptPairDays) {
    scanner::KlineCache cache(64);
    const std::vector<double> btcReturns{0.05, 0.10, -0.02, 0.08, 0.03};
    std::vector<double> coinReturns;
    coinReturns.reserve(btcReturns.size());
    for (double r : btcReturns) {
        coinReturns.push_back(2.0 * r);
    }

    auto btcCloses = closesFromReturns(100.0, btcReturns);
    auto coinCloses = closesFromReturns(200.0, coinReturns);
    coinCloses[3] = 0.0; // Corrupt one candle; paired returns touching this index must be skipped.

    seedDaily(cache, "BTCUSDT", btcCloses);
    seedDaily(cache, "ETHUSDT", coinCloses);

    engine::BetaCalculator calc;
    const auto beta = calc.calculate("ETHUSDT", cache, 30);
    ASSERT_TRUE(beta.has_value());
    EXPECT_NEAR(*beta, 2.0, 1e-6);
}
