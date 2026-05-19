#pragma once

#include "types/trade.h"

#include <map>
#include <string>
#include <vector>

struct Balance {
    std::string asset;
    double walletBalance{0.0};
    double crossWalletBalance{0.0};
    double unrealizedProfit{0.0};
    double marginBalance{0.0};
    double maintMargin{0.0};
    double initialMargin{0.0};
    double availableBalance{0.0};
    double maxWithdrawAmount{0.0};
};

struct Position {
    std::string symbol;
    PositionSide positionSide{PositionSide::Both};
    double positionAmt{0.0};
    double entryPrice{0.0};
    double breakEvenPrice{0.0};
    double markPrice{0.0};
    double unrealizedProfit{0.0};
    double liquidationPrice{0.0};
    int leverage{0};
    std::string marginType;
    double isolatedMargin{0.0};
    double initialMargin{0.0};
    double maintMargin{0.0};
    double notional{0.0};
};

struct FuturesAccount {
    double feeTier{0.0};
    bool canTrade{false};
    bool canDeposit{false};
    bool canWithdraw{false};
    double totalWalletBalance{0.0};
    double totalUnrealizedProfit{0.0};
    double totalMarginBalance{0.0};
    double totalInitialMargin{0.0};
    double totalMaintMargin{0.0};
    double availableBalance{0.0};
    double maxWithdrawAmount{0.0};
    std::vector<Balance> assets;
    std::vector<Position> positions;
};

struct AccountInfo {
    double totalBalance{0.0};
    std::map<std::string, double> balances;
};

struct LeverageResult {
    std::string symbol;
    int leverage{0};
    double maxNotionalValue{0.0};
};

struct FuturesAccountConfig {
    bool canTrade{false};
    bool dualSidePosition{false};
    bool multiAssetsMargin{false};
};

struct PositionModeStatus {
    bool dualSidePosition{false};
};

struct MultiAssetsModeStatus {
    bool multiAssetsMargin{false};
};

struct LeverageBracketTier {
    int bracket{0};
    int initialLeverage{0};
    double notionalCap{0.0};
    double notionalFloor{0.0};
    double maintMarginRatio{0.0};
    double cum{0.0};
};

struct SymbolLeverageBrackets {
    std::string symbol;
    std::vector<LeverageBracketTier> brackets;
};
