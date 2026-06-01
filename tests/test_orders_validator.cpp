#include <gtest/gtest.h>

#include "orders/order_types.h"
#include "orders/order_validator.h"

#include <algorithm>
#include <stdexcept>

namespace {

Quantity qty(std::string_view v) {
    auto parsed = Quantity::parse(v);
    if (!parsed) {
        throw std::runtime_error("invalid test quantity");
    }
    return *parsed;
}

Price price(std::string_view v) {
    auto parsed = Price::parse(v);
    if (!parsed) {
        throw std::runtime_error("invalid test price");
    }
    return *parsed;
}

bool hasIssueCode(const ValidationReport& report, std::string_view code) {
    return std::any_of(report.issues.begin(), report.issues.end(), [code](const ValidationIssue& issue) {
        return issue.code == code;
    });
}

} // namespace

TEST(OrderValidatorTest, CloseByMarketRequiresOneWayMode) {
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.positionMode = PositionMode::Hedge;
    OrderValidator validator(cfg);

    CloseByMarketDraft draft("BTCUSDT", OrderSide::Sell, qty("0.1"));

    auto report = validator.validateCloseByMarket(draft);
    EXPECT_TRUE(report.hasErrors());
}

TEST(OrderValidatorTest, MarketRejectsReduceOnlyInHedgeMode) {
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.positionMode = PositionMode::Hedge;
    OrderValidator validator(cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.1"),
        .positionSide = PositionSide::Long,
        .reduceOnly = true,
    };

    auto report = validator.validateMarket(draft);
    EXPECT_TRUE(report.hasErrors());
}

TEST(OrderValidatorTest, LimitRequiresPositivePrice) {
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.positionMode = PositionMode::OneWay;
    OrderValidator validator(cfg);

    auto zeroPrice = Price::parse("0");
    ASSERT_TRUE(zeroPrice.has_value());

    LimitOrderDraft draft{
        .symbol = "ETHUSDT",
        .side = OrderSide::Sell,
        .quantity = qty("0.5"),
        .price = *zeroPrice,
    };

    auto report = validator.validateLimit(draft);
    EXPECT_TRUE(report.hasErrors());
}

TEST(OrderValidatorTest, BatchHasMaximumFiveOrders) {
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.positionMode = PositionMode::OneWay;
    OrderValidator validator(cfg);

    std::vector<NormalOrderDraft> drafts;
    for (int i = 0; i < 6; ++i) {
        drafts.push_back(MarketOrderDraft{
            .symbol = "BTCUSDT",
            .side = OrderSide::Buy,
            .quantity = qty("0.01"),
        });
    }

    auto report = validator.validateBatch(drafts);
    EXPECT_TRUE(report.hasErrors());
}

TEST(OrderValidatorTest, BatchValidatesEachDraftContent) {
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.positionMode = PositionMode::OneWay;
    OrderValidator validator(cfg);

    auto invalidPrice = Price::parse("0");
    ASSERT_TRUE(invalidPrice.has_value());

    std::vector<NormalOrderDraft> drafts;
    drafts.push_back(MarketOrderDraft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
    });
    drafts.push_back(LimitOrderDraft{
        .symbol = "ETHUSDT",
        .side = OrderSide::Sell,
        .quantity = qty("0.02"),
        .price = *invalidPrice,
    });

    auto report = validator.validateBatch(drafts);
    EXPECT_TRUE(report.hasErrors());
}

TEST(OrderValidatorTest, RawTimestampOverrideDoesNotAllowSignatureBypass) {
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.positionMode = PositionMode::OneWay;
    cfg.allowRawTimestampOverride = true;
    OrderValidator validator(cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
        .raw = {
            {"timestamp", "1700000000000"},
            {"signature", "abc"},
            {"recvWindow", "3000"},
        },
    };

    auto report = validator.validateMarket(draft);
    EXPECT_TRUE(report.hasErrors());
}

TEST(OrderValidatorTest, RawParamKeyMustBeConservativeAscii) {
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.positionMode = PositionMode::OneWay;
    OrderValidator validator(cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
        .raw = {
            {"bad&key", "1"},
        },
    };

    auto report = validator.validateMarket(draft);
    EXPECT_TRUE(report.hasErrors());
}

TEST(OrderValidatorTest, RawModeledResponseTypeIsBlocked) {
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.positionMode = PositionMode::OneWay;
    OrderValidator validator(cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
        .raw = {
            {"newOrderRespType", "RESULT"},
        },
    };

    auto report = validator.validateMarket(draft);
    EXPECT_TRUE(report.hasErrors());
}

TEST(OrderValidatorTest, ValidatorAddsWarningAndSkippedAdvisories) {
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.positionMode = PositionMode::Unknown;
    OrderValidator validator(cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
    };

    auto report = validator.validateMarket(draft);
    bool foundWarning = false;
    bool foundSkipped = false;
    for (const auto& issue : report.issues) {
        if (issue.severity == ValidationIssue::Severity::Warning) {
            foundWarning = true;
        }
        if (issue.severity == ValidationIssue::Severity::Skipped) {
            foundSkipped = true;
        }
    }

    EXPECT_TRUE(foundWarning);
    EXPECT_TRUE(foundSkipped);
    EXPECT_FALSE(report.hasErrors());
}

TEST(OrderValidatorTest, RequiresExchangeInfoWhenConfigured) {
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.positionMode = PositionMode::OneWay;
    cfg.requireExchangeInfo = true;
    OrderValidator validator(cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
    };

    const auto report = validator.validateMarket(draft);
    EXPECT_TRUE(report.hasErrors());
    EXPECT_TRUE(hasIssueCode(report, "exchange_info_unavailable"));
}

TEST(OrderValidatorTest, RejectsOrdersOutsideExchangeFilters) {
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.positionMode = PositionMode::OneWay;
    cfg.requireExchangeInfo = true;
    cfg.exchangeInfoBySymbol.emplace(
        "BTCUSDT",
        ExchangeSymbol{
            .symbol = "BTCUSDT",
            .tickSize = 0.10,
            .stepSize = 0.001,
            .minNotional = 5.0,
            .maxQty = 100.0,
            .minQty = 0.001,
        });
    OrderValidator validator(cfg);

    LimitOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.0015"),
        .price = price("100.05"),
    };

    const auto report = validator.validateLimit(draft);
    EXPECT_TRUE(report.hasErrors());
    EXPECT_TRUE(hasIssueCode(report, "quantity_step_size_mismatch"));
    EXPECT_TRUE(hasIssueCode(report, "limit_price_tick_size_mismatch"));
    EXPECT_TRUE(hasIssueCode(report, "notional_below_min_notional"));
}

TEST(OrderValidatorTest, RejectsStaleExchangeInfoWhenRequired) {
    OrdersConfig cfg;
    cfg.clientIdNamespace = "test";
    cfg.positionMode = PositionMode::OneWay;
    cfg.requireExchangeInfo = true;
    cfg.exchangeInfoBySymbol.emplace(
        "BTCUSDT",
        ExchangeSymbol{
            .symbol = "BTCUSDT",
            .tickSize = 0.10,
            .stepSize = 0.001,
            .minNotional = 5.0,
            .maxQty = 100.0,
            .minQty = 0.001,
        });
    cfg.exchangeInfoUpdatedAt = std::chrono::system_clock::now() - std::chrono::hours{2};
    cfg.exchangeInfoMaxAge = std::chrono::minutes{30};
    OrderValidator validator(cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.01"),
    };

    const auto report = validator.validateMarket(draft);
    EXPECT_TRUE(report.hasErrors());
    EXPECT_TRUE(hasIssueCode(report, "exchange_info_stale"));
}
