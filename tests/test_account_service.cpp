#include <gtest/gtest.h>

#include "account/account_service.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace {

class StubAccountRestClient final : public account::IAccountRestClient {
public:
    account::AccountRestResult<FuturesAccount> accountResult = FuturesAccount{};
    account::AccountRestResult<std::vector<Balance>> balanceResult = std::vector<Balance>{};
    account::AccountRestResult<std::vector<Position>> positionsResult = std::vector<Position>{};
    account::AccountRestResult<FuturesAccountConfig> accountConfigResult = FuturesAccountConfig{};
    account::AccountRestResult<void> testOrderResult = account::AccountRestResult<void>{};

    int accountCalls{0};
    int balanceCalls{0};
    int positionsCalls{0};
    int accountConfigCalls{0};
    int testOrderCalls{0};
    std::optional<std::string> lastPositionFilter;
    std::optional<OrderRequest> lastTestOrderRequest;

    boost::asio::awaitable<account::AccountRestResult<FuturesAccount>> account() override {
        ++accountCalls;
        co_return accountResult;
    }

    boost::asio::awaitable<account::AccountRestResult<std::vector<Balance>>> balance() override {
        ++balanceCalls;
        co_return balanceResult;
    }

    boost::asio::awaitable<account::AccountRestResult<std::vector<Position>>> positions(
        std::optional<std::string> symbol) override {
        ++positionsCalls;
        lastPositionFilter = std::move(symbol);
        co_return positionsResult;
    }

    boost::asio::awaitable<account::AccountRestResult<FuturesAccountConfig>> accountConfig() override {
        ++accountConfigCalls;
        co_return accountConfigResult;
    }

    boost::asio::awaitable<account::AccountRestResult<void>> testOrder(OrderRequest req) override {
        ++testOrderCalls;
        lastTestOrderRequest = std::move(req);
        co_return testOrderResult;
    }
};

template <typename T>
T runAwaitable(boost::asio::awaitable<T> task) {
    boost::asio::io_context ioc;
    auto fut = boost::asio::co_spawn(ioc, std::move(task), boost::asio::use_future);
    ioc.run();
    return fut.get();
}

Quantity qty(std::string_view value) {
    auto parsed = Quantity::parse(value);
    if (!parsed) {
        throw std::runtime_error("invalid quantity");
    }
    return *parsed;
}

Price price(std::string_view value) {
    auto parsed = Price::parse(value);
    if (!parsed) {
        throw std::runtime_error("invalid price");
    }
    return *parsed;
}

} // namespace

TEST(AccountServiceTest, SnapshotIncludeAccountConfigKeepsPartialCompletenessAndLoadsModeFlags) {
    StubAccountRestClient rest;
    FuturesAccount account;
    account.canTrade = true;
    rest.accountResult = account;
    rest.accountConfigResult = FuturesAccountConfig{
        .canTrade = false,
        .dualSidePosition = true,
        .multiAssetsMargin = true,
    };
    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    account::AccountSnapshotRequest request;
    request.includeAccountConfig = true;

    auto result = runAwaitable(service.snapshot(request));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->completeness, account::AccountSnapshotCompleteness::AccountOnly);
    ASSERT_TRUE(result->dualSidePosition.has_value());
    EXPECT_TRUE(*result->dualSidePosition);
    ASSERT_TRUE(result->multiAssetsMargin.has_value());
    EXPECT_TRUE(*result->multiAssetsMargin);
    EXPECT_FALSE(result->account.canTrade);
    EXPECT_EQ(rest.accountCalls, 1);
    EXPECT_EQ(rest.accountConfigCalls, 1);
}

TEST(AccountServiceTest, SnapshotFullCompletenessRequiresBalanceAndPositionsPlusConfig) {
    StubAccountRestClient rest;
    rest.accountResult = FuturesAccount{};
    rest.balanceResult = std::vector<Balance>{Balance{.asset = "USDT"}};
    rest.positionsResult = std::vector<Position>{Position{.symbol = "BTCUSDT"}};
    rest.accountConfigResult = FuturesAccountConfig{
        .canTrade = true,
        .dualSidePosition = true,
        .multiAssetsMargin = false,
    };
    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    account::AccountSnapshotRequest request;
    request.includeBalanceEndpoint = true;
    request.includePositions = true;
    request.includeAccountConfig = true;

    auto result = runAwaitable(service.snapshot(request));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->completeness, account::AccountSnapshotCompleteness::Full);
}

TEST(AccountServiceTest, SnapshotConfigCanTradeAndAccountCanTradeAreCombinedConservatively) {
    StubAccountRestClient rest;
    FuturesAccount account;
    account.canTrade = false;
    rest.accountResult = account;
    rest.accountConfigResult = FuturesAccountConfig{
        .canTrade = true,
        .dualSidePosition = false,
        .multiAssetsMargin = false,
    };
    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    account::AccountSnapshotRequest request;
    request.includeAccountConfig = true;

    auto result = runAwaitable(service.snapshot(request));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->account.canTrade);
}

TEST(AccountServiceTest, SnapshotPropagatesAccountRestFailure) {
    StubAccountRestClient rest;
    rest.accountResult = std::unexpected(BinanceError::fromApiResponse(-1000, "account unavailable"));

    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    auto result = runAwaitable(service.snapshot());
    ASSERT_FALSE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<BinanceError>(result.error()));
    const auto& error = std::get<BinanceError>(result.error());
    EXPECT_EQ(error.code, -1000);
    EXPECT_EQ(error.message, "account unavailable");
    EXPECT_EQ(rest.accountCalls, 1);
    EXPECT_EQ(rest.balanceCalls, 0);
    EXPECT_EQ(rest.positionsCalls, 0);
}

TEST(AccountServiceTest, SnapshotWithPositionsOnlyMarksAccountAndPositions) {
    StubAccountRestClient rest;
    FuturesAccount account;
    account.canTrade = true;
    rest.accountResult = account;
    Position position;
    position.symbol = "BTCUSDT";
    rest.positionsResult = std::vector<Position>{position};

    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    account::AccountSnapshotRequest request;
    request.includePositions = true;
    request.positionFilter = "BTCUSDT";

    auto result = runAwaitable(service.snapshot(request));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->completeness, account::AccountSnapshotCompleteness::AccountAndPositions);
    ASSERT_TRUE(result->positions.has_value());
    EXPECT_EQ(result->positions->size(), 1u);
    EXPECT_EQ(rest.accountCalls, 1);
    EXPECT_EQ(rest.positionsCalls, 1);
    EXPECT_EQ(rest.lastPositionFilter, std::optional<std::string>("BTCUSDT"));
}

TEST(AccountServiceTest, SnapshotFetchesFilteredPositionsWhenFilterProvided) {
    StubAccountRestClient rest;
    rest.accountResult = FuturesAccount{};
    Position position;
    position.symbol = "BTCUSDT";
    rest.positionsResult = std::vector<Position>{position};

    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    account::AccountSnapshotRequest request;
    request.positionFilter = "BTCUSDT";

    auto result = runAwaitable(service.snapshot(request));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->positions.has_value());
    EXPECT_EQ(result->positions->size(), 1u);
    EXPECT_EQ(rest.positionsCalls, 1);
    EXPECT_EQ(rest.lastPositionFilter, std::optional<std::string>("BTCUSDT"));
}

TEST(AccountServiceTest, SnapshotWithBalanceAndPositionsMarksAccountBalanceAndPositions) {
    StubAccountRestClient rest;
    rest.accountResult = FuturesAccount{};
    Balance balance;
    balance.asset = "USDT";
    rest.balanceResult = std::vector<Balance>{balance};
    rest.positionsResult = std::vector<Position>{};

    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    account::AccountSnapshotRequest request;
    request.includeBalanceEndpoint = true;
    request.includePositions = true;

    auto result = runAwaitable(service.snapshot(request));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->completeness, account::AccountSnapshotCompleteness::AccountBalanceAndPositions);
    ASSERT_TRUE(result->balances.has_value());
    ASSERT_TRUE(result->positions.has_value());
    EXPECT_EQ(rest.accountCalls, 1);
    EXPECT_EQ(rest.balanceCalls, 1);
    EXPECT_EQ(rest.positionsCalls, 1);
}

TEST(AccountServiceTest, CheckFreeMarginValidatesViaTestOrder) {
    StubAccountRestClient rest;
    rest.accountResult = FuturesAccount{};
    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    account::MarginCheckDraft draft{
        .symbol = "BTCUSDT",
        .side = account::MarginCheckSide::Buy,
        .quantity = qty("0.01"),
    };

    auto result = runAwaitable(service.checkFreeMargin(draft));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->completeness, account::MarginCheckCompleteness::ServerValidatedOnly);
    EXPECT_TRUE(result->serverAccepted);
    EXPECT_EQ(rest.testOrderCalls, 1);
    ASSERT_TRUE(rest.lastTestOrderRequest.has_value());
    EXPECT_EQ(rest.lastTestOrderRequest->symbol, "BTCUSDT");
    EXPECT_EQ(rest.lastTestOrderRequest->type, OrderType::Market);
}

TEST(AccountServiceTest, CheckFreeMarginBothOptionsKeepsServerValidatedOnly) {
    StubAccountRestClient rest;
    FuturesAccount account;
    account.availableBalance = 1000.0;
    account.availableBalanceRaw = "1000";
    Position position;
    position.symbol = "BTCUSDT";
    position.leverage = 10;
    account.positions.push_back(position);
    rest.accountResult = account;
    ASSERT_TRUE(rest.accountResult.has_value());
    EXPECT_EQ(rest.accountResult->availableBalanceRaw, "1000");

    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    account::MarginCheckDraft draft{
        .symbol = "BTCUSDT",
        .side = account::MarginCheckSide::Buy,
        .quantity = qty("1"),
        .assumedPrice = price("100"),
        .useServerTestOrder = true,
    };

    auto result = runAwaitable(service.checkFreeMargin(draft));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->completeness, account::MarginCheckCompleteness::ServerValidatedOnly);
    EXPECT_TRUE(result->serverAccepted);
    ASSERT_TRUE(result->estimatedRemainingFreeMargin.has_value());
    EXPECT_EQ(*result->estimatedRemainingFreeMargin, "990.0");
    EXPECT_EQ(rest.testOrderCalls, 1);
    EXPECT_EQ(rest.accountCalls, 1);
}

TEST(AccountServiceTest, CheckFreeMarginLocalEstimateUsesAvailableBalanceRawWhenPresent) {
    StubAccountRestClient rest;
    FuturesAccount account;
    account.availableBalance = 1000.0;
    account.availableBalanceRaw = "500";
    Position position;
    position.symbol = "BTCUSDT";
    position.leverage = 10;
    account.positions.push_back(position);
    rest.accountResult = account;

    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    account::MarginCheckDraft draft{
        .symbol = "BTCUSDT",
        .side = account::MarginCheckSide::Buy,
        .quantity = qty("1"),
        .assumedPrice = price("100"),
        .useServerTestOrder = false,
    };

    auto result = runAwaitable(service.checkFreeMargin(draft));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->completeness, account::MarginCheckCompleteness::Estimated);
    ASSERT_TRUE(result->estimatedRemainingFreeMargin.has_value());
    EXPECT_EQ(*result->estimatedRemainingFreeMargin, "490.0");
}

TEST(AccountServiceTest, CheckFreeMarginLocalEstimateUnavailableWhenQuantityNonPositive) {
    StubAccountRestClient rest;
    FuturesAccount account;
    account.availableBalance = 1000.0;
    account.availableBalanceRaw = "1000";
    Position position;
    position.symbol = "BTCUSDT";
    position.leverage = 10;
    account.positions.push_back(position);
    rest.accountResult = account;

    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    account::MarginCheckDraft draft{
        .symbol = "BTCUSDT",
        .side = account::MarginCheckSide::Buy,
        .quantity = qty("0"),
        .assumedPrice = price("100"),
        .useServerTestOrder = false,
    };

    auto result = runAwaitable(service.checkFreeMargin(draft));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->completeness, account::MarginCheckCompleteness::Unavailable);
    EXPECT_FALSE(result->estimatedRemainingFreeMargin.has_value());
    EXPECT_EQ(rest.accountCalls, 0);
}

TEST(AccountServiceTest, CheckFreeMarginLocalEstimateUnavailableWhenAssumedPriceNonPositive) {
    StubAccountRestClient rest;
    FuturesAccount account;
    account.availableBalance = 1000.0;
    account.availableBalanceRaw = "1000";
    Position position;
    position.symbol = "BTCUSDT";
    position.leverage = 10;
    account.positions.push_back(position);
    rest.accountResult = account;

    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    account::MarginCheckDraft draft{
        .symbol = "BTCUSDT",
        .side = account::MarginCheckSide::Buy,
        .quantity = qty("1"),
        .assumedPrice = price("0"),
        .useServerTestOrder = false,
    };

    auto result = runAwaitable(service.checkFreeMargin(draft));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->completeness, account::MarginCheckCompleteness::Unavailable);
    EXPECT_FALSE(result->estimatedRemainingFreeMargin.has_value());
    EXPECT_EQ(rest.accountCalls, 0);
}

TEST(AccountServiceTest, CheckFreeMarginLocalEstimateUnavailableWhenAvailableBalanceRawInvalid) {
    StubAccountRestClient rest;
    FuturesAccount account;
    account.availableBalance = 1000.0;
    account.availableBalanceRaw = "invalid";
    Position position;
    position.symbol = "BTCUSDT";
    position.leverage = 10;
    account.positions.push_back(position);
    rest.accountResult = account;

    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    account::MarginCheckDraft draft{
        .symbol = "BTCUSDT",
        .side = account::MarginCheckSide::Buy,
        .quantity = qty("1"),
        .assumedPrice = price("100"),
        .useServerTestOrder = false,
    };

    auto result = runAwaitable(service.checkFreeMargin(draft));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->completeness, account::MarginCheckCompleteness::Unavailable);
    EXPECT_FALSE(result->estimatedRemainingFreeMargin.has_value());
}

TEST(AccountServiceTest, CheckFreeMarginRejectsEmptyOptionsWithUnsupportedError) {
    StubAccountRestClient rest;
    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    account::MarginCheckDraft draft{
        .symbol = "BTCUSDT",
        .side = account::MarginCheckSide::Buy,
        .quantity = qty("1"),
        .assumedPrice = std::nullopt,
        .useServerTestOrder = false,
    };

    auto result = runAwaitable(service.checkFreeMargin(draft));
    ASSERT_FALSE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<account::AccountMappingError>(result.error()));
    EXPECT_EQ(std::get<account::AccountMappingError>(result.error()), account::AccountMappingError::Unsupported);
    EXPECT_EQ(rest.testOrderCalls, 0);
    EXPECT_EQ(rest.accountCalls, 0);
}

TEST(AccountServiceTest, LiquidationRiskReturnsPositionOnlyView) {
    StubAccountRestClient rest;
    Position position;
    position.symbol = "BTCUSDT";
    rest.positionsResult = std::vector<Position>{position};
    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    auto result = runAwaitable(service.liquidationRisk("BTCUSDT"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->completeness, account::LiquidationRiskCompleteness::PositionOnly);
    ASSERT_EQ(result->positions.size(), 1u);
    EXPECT_EQ(result->positions.front().symbol, "BTCUSDT");
    EXPECT_EQ(rest.positionsCalls, 1);
}

TEST(AccountServiceTest, LiquidationRiskPropagatesRestFailure) {
    StubAccountRestClient rest;
    rest.positionsResult = std::unexpected(BinanceError::fromApiResponse(-2011, "position risk unavailable"));
    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    auto result = runAwaitable(service.liquidationRisk("BTCUSDT"));
    ASSERT_FALSE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<BinanceError>(result.error()));
    const auto& error = std::get<BinanceError>(result.error());
    EXPECT_EQ(error.code, -2011);
    EXPECT_EQ(error.message, "position risk unavailable");
}

TEST(AccountServiceTest, LiquidationRiskEmptyRowsDifferByScope) {
    StubAccountRestClient rest;
    rest.positionsResult = std::vector<Position>{};
    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    auto symbolScoped = runAwaitable(service.liquidationRisk("BTCUSDT"));
    ASSERT_TRUE(symbolScoped.has_value());
    ASSERT_TRUE(symbolScoped->note.has_value());
    EXPECT_EQ(*symbolScoped->note, "No position risk rows found for requested symbol");

    auto accountScoped = runAwaitable(service.liquidationRisk());
    ASSERT_TRUE(accountScoped.has_value());
    ASSERT_TRUE(accountScoped->note.has_value());
    EXPECT_EQ(*accountScoped->note, "No position risk rows found for account scope");
}
