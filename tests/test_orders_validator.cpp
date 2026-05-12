#include <gtest/gtest.h>

#include "orders/order_types.h"
#include "orders/order_validator.h"

#include <stdexcept>

namespace {

Quantity qty(std::string_view v) {
    auto parsed = Quantity::parse(v);
    if (!parsed) {
        throw std::runtime_error("invalid test quantity");
    }
    return *parsed;
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
