#include <gtest/gtest.h>
#include "account/mql4_account_adapter.h"
#include <chrono>

using namespace account;
using namespace account::mql4;

class Mql4AccountAdapterTest : public ::testing::Test {
protected:
    AccountSnapshot createBasicSnapshot() {
        FuturesAccount acc;
        acc.totalWalletBalance = 10000.0;
        acc.totalUnrealizedProfit = 500.0;
        acc.totalMarginBalance = 10500.0;
        acc.totalInitialMargin = 1000.0;
        acc.availableBalance = 9500.0;
        acc.canTrade = true;

        Balance usdt;
        usdt.asset = "USDT";
        usdt.walletBalance = 10000.0;
        usdt.unrealizedProfit = 500.0;
        usdt.marginBalance = 10500.0;
        usdt.initialMargin = 1000.0;
        usdt.availableBalance = 9500.0;
        acc.assets.push_back(usdt);

        Position btc;
        btc.symbol = "BTCUSDT";
        btc.leverage = 20;
        btc.initialMargin = 1000.0;
        acc.positions.push_back(btc);

        AccountCompatibilityConfig cfg;
        cfg.displayAsset = "USDT";
        cfg.company = "Binance";
        cfg.serverName = "Production";
        cfg.loginOverride = 123456;

        AccountSnapshot snapshot;
        snapshot.capturedAt = std::chrono::system_clock::now();
        snapshot.account = std::move(acc);
        snapshot.compatibility = std::move(cfg);
        return snapshot;
    }
};

TEST_F(Mql4AccountAdapterTest, BasicProperties) {
    auto snapshot = createBasicSnapshot();
    Mql4AccountAdapter adapter(std::move(snapshot));

    EXPECT_EQ(*adapter.accountBalance(), 10000.0);
    EXPECT_EQ(*adapter.accountProfit(), 500.0);
    EXPECT_EQ(*adapter.accountEquity(), 10500.0);
    EXPECT_EQ(*adapter.accountMargin(), 1000.0);
    EXPECT_EQ(*adapter.accountFreeMargin(), 9500.0);
    EXPECT_EQ(*adapter.accountCompany(), "Binance");
    EXPECT_EQ(*adapter.accountCurrency(), "USDT");
    EXPECT_EQ(*adapter.accountServer(), "Production");
    EXPECT_EQ(*adapter.accountNumber(), 123456);
}

TEST_F(Mql4AccountAdapterTest, AccountInfoDouble) {
    auto snapshot = createBasicSnapshot();
    Mql4AccountAdapter adapter(std::move(snapshot));

    EXPECT_EQ(*adapter.accountInfoDouble(AccountDoubleProperty::Balance), 10000.0);
    EXPECT_EQ(*adapter.accountInfoDouble(AccountDoubleProperty::Profit), 500.0);
    EXPECT_EQ(*adapter.accountInfoDouble(AccountDoubleProperty::Equity), 10500.0);
    EXPECT_NEAR(*adapter.accountInfoDouble(AccountDoubleProperty::MarginLevel), 1050.0, 0.001); // 10500 / 1000 * 100
}

TEST_F(Mql4AccountAdapterTest, AccountInfoInteger) {
    auto snapshot = createBasicSnapshot();
    Mql4AccountAdapter adapter(std::move(snapshot));

    EXPECT_EQ(*adapter.accountInfoInteger(AccountIntegerProperty::Login), 123456);
    EXPECT_EQ(*adapter.accountInfoInteger(AccountIntegerProperty::TradeAllowed), 1);
}

TEST_F(Mql4AccountAdapterTest, LeverageBySymbol) {
    auto snapshot = createBasicSnapshot();
    Mql4AccountAdapter adapter(std::move(snapshot));

    EXPECT_EQ(*adapter.accountLeverage("BTCUSDT"), 20);
    EXPECT_EQ(*adapter.accountLeverage("btcusdt"), 20);
    auto missingSymbol = adapter.accountLeverage("ETHUSDT");
    EXPECT_FALSE(missingSymbol.has_value());
    EXPECT_EQ(missingSymbol.error(), AccountMappingError::SnapshotIncomplete);
}

TEST_F(Mql4AccountAdapterTest, LeverageWithoutSymbolIsAmbiguous) {
    auto snapshot = createBasicSnapshot();
    Mql4AccountAdapter adapter(std::move(snapshot));

    auto leverage = adapter.accountLeverage("");
    EXPECT_FALSE(leverage.has_value());
    EXPECT_EQ(leverage.error(), AccountMappingError::AmbiguousSymbol);
}

TEST_F(Mql4AccountAdapterTest, LeverageUsesPositionRiskSnapshotEvenWhenPositionAmtIsZero) {
    auto snapshot = createBasicSnapshot();
    snapshot.account.positions.clear();
    Position riskOnly;
    riskOnly.symbol = "ETHUSDT";
    riskOnly.positionAmt = 0.0;
    riskOnly.leverage = 25;
    snapshot.positions = std::vector<Position>{riskOnly};

    Mql4AccountAdapter adapter(std::move(snapshot));
    auto leverage = adapter.accountLeverage("ethusdt");
    ASSERT_TRUE(leverage.has_value());
    EXPECT_EQ(*leverage, 25);
}

TEST_F(Mql4AccountAdapterTest, UnsupportedProperties) {
    auto snapshot = createBasicSnapshot();
    Mql4AccountAdapter adapter(std::move(snapshot));

    EXPECT_FALSE(adapter.accountInfoDouble(AccountDoubleProperty::MarginStopoutCall).has_value());
    EXPECT_FALSE(adapter.accountInfoInteger(AccountIntegerProperty::LimitOrders).has_value());
}

TEST_F(Mql4AccountAdapterTest, AccountCreditPolicyExplicitOnlyIsUnsupported) {
    auto snapshot = createBasicSnapshot();
    snapshot.compatibility.creditPolicy = AccountCreditPolicy::ExplicitOnly;
    Mql4AccountAdapter adapter(std::move(snapshot));

    auto credit = adapter.accountCredit();
    EXPECT_FALSE(credit.has_value());
    EXPECT_EQ(credit.error(), AccountMappingError::Unsupported);
}

TEST_F(Mql4AccountAdapterTest, AccountCreditPolicyAssumeZeroReturnsZero) {
    auto snapshot = createBasicSnapshot();
    snapshot.compatibility.creditPolicy = AccountCreditPolicy::AssumeZero;
    Mql4AccountAdapter adapter(std::move(snapshot));

    auto credit = adapter.accountCredit();
    ASSERT_TRUE(credit.has_value());
    EXPECT_DOUBLE_EQ(*credit, 0.0);
}

TEST_F(Mql4AccountAdapterTest, AccountNumberFailsWhenLoginOverrideAbsent) {
    auto snapshot = createBasicSnapshot();
    snapshot.compatibility.loginOverride.reset();
    Mql4AccountAdapter adapter(std::move(snapshot));

    auto accountNumber = adapter.accountNumber();
    EXPECT_FALSE(accountNumber.has_value());
    EXPECT_EQ(accountNumber.error(), AccountMappingError::NotConfigured);
}

TEST_F(Mql4AccountAdapterTest, AccountBalanceFailsSnapshotIncompleteWhenDisplayAssetMissing) {
    auto snapshot = createBasicSnapshot();
    snapshot.compatibility.displayAsset = "BNB";
    Mql4AccountAdapter adapter(std::move(snapshot));

    auto balance = adapter.accountBalance();
    EXPECT_FALSE(balance.has_value());
    EXPECT_EQ(balance.error(), AccountMappingError::SnapshotIncomplete);
}

TEST_F(Mql4AccountAdapterTest, AccountBalanceFailsNotConfiguredWhenDisplayAssetEmpty) {
    auto snapshot = createBasicSnapshot();
    snapshot.compatibility.displayAsset.clear();
    Mql4AccountAdapter adapter(std::move(snapshot));

    auto balance = adapter.accountBalance();
    EXPECT_FALSE(balance.has_value());
    EXPECT_EQ(balance.error(), AccountMappingError::NotConfigured);
}

TEST_F(Mql4AccountAdapterTest, UsesDisplayAssetValueInsteadOfAccountTotals) {
    auto snapshot = createBasicSnapshot();
    snapshot.account.totalWalletBalance = 99999.0;
    snapshot.account.totalMarginBalance = 88888.0;
    snapshot.account.availableBalance = 77777.0;
    snapshot.account.totalInitialMargin = 66666.0;
    snapshot.account.totalUnrealizedProfit = 55555.0;

    Mql4AccountAdapter adapter(std::move(snapshot));

    EXPECT_DOUBLE_EQ(*adapter.accountBalance(), 10000.0);
    EXPECT_DOUBLE_EQ(*adapter.accountEquity(), 10500.0);
    EXPECT_DOUBLE_EQ(*adapter.accountFreeMargin(), 9500.0);
    EXPECT_DOUBLE_EQ(*adapter.accountMargin(), 1000.0);
    EXPECT_DOUBLE_EQ(*adapter.accountProfit(), 500.0);
}

TEST_F(Mql4AccountAdapterTest, FreeMarginIgnoresAvailableBalanceReserveWhenNoPositions) {
    auto snapshot = createBasicSnapshot();
    snapshot.account.positions.clear();
    snapshot.account.assets[0].initialMargin = 0.0;
    snapshot.account.assets[0].availableBalance = 9300.0;
    snapshot.account.assets[0].marginBalance = 10500.0;

    Mql4AccountAdapter adapter(std::move(snapshot));
    auto freeMargin = adapter.accountFreeMargin();
    ASSERT_TRUE(freeMargin.has_value());
    EXPECT_DOUBLE_EQ(*freeMargin, 10500.0);
}

TEST_F(Mql4AccountAdapterTest, MultiAssetMode) {
    FuturesAccount acc;
    acc.totalWalletBalance = 20000.0; // Total in USD aggregate
    acc.canTrade = true;

    Balance usdt;
    usdt.asset = "USDT";
    usdt.walletBalance = 10000.0;
    acc.assets.push_back(usdt);

    Balance bnb;
    bnb.asset = "BNB";
    bnb.walletBalance = 50.0;
    acc.assets.push_back(bnb);

    AccountCompatibilityConfig cfg;
    cfg.displayAsset = "BNB"; // We want to see BNB balance
    
    AccountSnapshot snapshot;
    snapshot.account = std::move(acc);
    snapshot.compatibility = std::move(cfg);
    
    Mql4AccountAdapter adapter(std::move(snapshot));
    EXPECT_EQ(*adapter.accountBalance(), 50.0);
    EXPECT_EQ(*adapter.accountCurrency(), "BNB");
}

TEST_F(Mql4AccountAdapterTest, MultiAssetsMarginIsUnsupportedForSingleAssetMappings) {
    auto snapshot = createBasicSnapshot();
    snapshot.multiAssetsMargin = true;
    Mql4AccountAdapter adapter(std::move(snapshot));

    auto balance = adapter.accountBalance();
    EXPECT_FALSE(balance.has_value());
    EXPECT_EQ(balance.error(), AccountMappingError::Unsupported);

    auto freeMargin = adapter.accountFreeMargin();
    EXPECT_FALSE(freeMargin.has_value());
    EXPECT_EQ(freeMargin.error(), AccountMappingError::Unsupported);
}
