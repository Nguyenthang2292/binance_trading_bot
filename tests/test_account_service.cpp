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

class StubAccountRestClient final : public IAccountRestClient {
public:
    AccountRestResult<FuturesAccount> accountResult = FuturesAccount{};
    AccountRestResult<std::vector<Balance>> balanceResult = std::vector<Balance>{};
    AccountRestResult<std::vector<Position>> positionsResult = std::vector<Position>{};

    int accountCalls{0};
    int balanceCalls{0};
    int positionsCalls{0};
    std::optional<std::string> lastPositionFilter;

    boost::asio::awaitable<AccountRestResult<FuturesAccount>> account() override {
        ++accountCalls;
        co_return accountResult;
    }

    boost::asio::awaitable<AccountRestResult<std::vector<Balance>>> balance() override {
        ++balanceCalls;
        co_return balanceResult;
    }

    boost::asio::awaitable<AccountRestResult<std::vector<Position>>> positions(
        std::optional<std::string> symbol) override {
        ++positionsCalls;
        lastPositionFilter = std::move(symbol);
        co_return positionsResult;
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

} // namespace

TEST(AccountServiceTest, SnapshotRejectsIncludeAccountConfigBeforePhaseD) {
    StubAccountRestClient rest;
    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    account::AccountSnapshotRequest request;
    request.includeAccountConfig = true;

    auto result = runAwaitable(service.snapshot(request));
    ASSERT_FALSE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<account::AccountMappingError>(result.error()));
    EXPECT_EQ(std::get<account::AccountMappingError>(result.error()), account::AccountMappingError::Unsupported);
    EXPECT_EQ(rest.accountCalls, 0);
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

TEST(AccountServiceTest, CheckFreeMarginPhaseCStubReturnsUnavailableCompleteness) {
    StubAccountRestClient rest;
    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    account::MarginCheckDraft draft{
        .symbol = "BTCUSDT",
        .side = account::MarginCheckSide::Buy,
        .quantity = qty("0.01"),
    };

    auto result = runAwaitable(service.checkFreeMargin(draft));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol, "BTCUSDT");
    EXPECT_EQ(result->completeness, account::MarginCheckCompleteness::Unavailable);
    ASSERT_TRUE(result->quantity.has_value());
    EXPECT_EQ(result->quantity->value(), "0.01");
}

TEST(AccountServiceTest, LiquidationRiskPhaseCStubReturnsUnsupported) {
    StubAccountRestClient rest;
    account::AccountCompatibilityConfig cfg;
    account::AccountService service(rest, cfg);

    auto result = runAwaitable(service.liquidationRisk("BTCUSDT"));
    ASSERT_FALSE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<account::AccountMappingError>(result.error()));
    EXPECT_EQ(std::get<account::AccountMappingError>(result.error()), account::AccountMappingError::Unsupported);
}
