#include <gtest/gtest.h>

#include "orders/order_mapper.h"

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

} // namespace

TEST(OrderMapperTest, PreservesMarketQuantityString) {
    OrdersConfig cfg;
    cfg.defaultResponseType = ResponseType::ACK;
    cfg.recvWindow = std::chrono::milliseconds(5000);
    OrderMapper mapper(cfg);

    MarketOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Buy,
        .quantity = qty("0.00100000"),
    };

    auto req = mapper.toOrderRequest(draft, "cid-1");
    EXPECT_EQ(req.quantity, "0.00100000");
}

TEST(OrderMapperTest, PreservesLimitPriceAndQuantityStrings) {
    OrdersConfig cfg;
    cfg.defaultResponseType = ResponseType::RESULT;
    cfg.recvWindow = std::chrono::milliseconds(5000);
    OrderMapper mapper(cfg);

    LimitOrderDraft draft{
        .symbol = "BTCUSDT",
        .side = OrderSide::Sell,
        .quantity = qty("0.0500"),
        .price = price("12345.6700"),
    };

    auto req = mapper.toOrderRequest(draft, "cid-2");
    EXPECT_EQ(req.quantity, "0.0500");
    ASSERT_TRUE(req.price.has_value());
    EXPECT_EQ(*req.price, "12345.6700");
}
