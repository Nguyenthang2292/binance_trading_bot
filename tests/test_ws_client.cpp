#include <gtest/gtest.h>

#include "ws/ws_client.h"

TEST(WsClientTest, CanRegisterAndRemoveSubscriptions) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl(boost::asio::ssl::context::tls_client);
    ContextConfig cfg;
    cfg.testnet = true;

    WsClient ws(ioc, ssl, cfg);

    ws.subscribeAggTrade("BTCUSDT", [](auto, auto) {});
    ws.subscribeKline("BTCUSDT", "1m", [](auto, auto) {});
    ws.subscribeMarkPrice("BTCUSDT", [](auto, auto) {});
    ws.subscribeBookTicker("BTCUSDT", [](auto, auto) {});
    ws.subscribeDepth("BTCUSDT", 20, "100ms", [](auto, auto) {});
    ws.subscribeLiquidation("BTCUSDT", [](auto, auto) {});
    ws.subscribeCompositeIndex("BTCUSDT", [](auto, auto) {});

    ws.unsubscribe("btcusdt@aggTrade");
    ws.unsubscribeAll();

    SUCCEED();
}
