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

TEST(WsClientTest, BuildsMarketRoutedPathForKlineStreams) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl(boost::asio::ssl::context::tls_client);
    ContextConfig cfg;
    cfg.testnet = true;

    WsClient ws(ioc, ssl, cfg);
    ws.subscribeKline("BTCUSDT", "30m", [](auto, auto) {});

    EXPECT_EQ(ws.streamPathForDiagnostics(), "/market/stream?streams=btcusdt@kline_30m");
}

TEST(WsClientTest, BuildsPublicRoutedPathForDepthAndBookTickerStreams) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl(boost::asio::ssl::context::tls_client);
    ContextConfig cfg;
    cfg.testnet = true;

    WsClient ws(ioc, ssl, cfg);
    ws.subscribeBookTicker("BTCUSDT", [](auto, auto) {});
    ws.subscribeDepth("ETHUSDT", 20, "100ms", [](auto, auto) {});

    const auto path = ws.streamPathForDiagnostics();
    EXPECT_EQ(path.find("/public/stream?streams="), 0u);
    EXPECT_NE(path.find("btcusdt@bookticker"), std::string::npos);
    EXPECT_NE(path.find("ethusdt@depth20@100ms"), std::string::npos);
}

TEST(WsClientTest, RejectsMixedPublicAndMarketRoutedPath) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl(boost::asio::ssl::context::tls_client);
    ContextConfig cfg;
    cfg.testnet = true;

    WsClient ws(ioc, ssl, cfg);
    ws.subscribeKline("BTCUSDT", "30m", [](auto, auto) {});
    ws.subscribeDepth("BTCUSDT", 20, "100ms", [](auto, auto) {});

    EXPECT_TRUE(ws.streamPathForDiagnostics().empty());
}
