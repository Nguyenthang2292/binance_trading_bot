#include "account/rest_account_client_adapter.h"

#include <utility>

boost::asio::awaitable<AccountRestResult<FuturesAccount>> AccountRestClientAdapter::account() {
    co_return co_await m_client.account();
}

boost::asio::awaitable<AccountRestResult<std::vector<Balance>>> AccountRestClientAdapter::balance() {
    co_return co_await m_client.balance();
}

boost::asio::awaitable<AccountRestResult<std::vector<Position>>> AccountRestClientAdapter::positions(
    std::optional<std::string> symbol) {
    co_return co_await m_client.positions(std::move(symbol));
}

boost::asio::awaitable<AccountRestResult<FuturesAccountConfig>> AccountRestClientAdapter::accountConfig() {
    co_return co_await m_client.accountConfig();
}

boost::asio::awaitable<AccountRestResult<void>> AccountRestClientAdapter::testOrder(OrderRequest req) {
    co_return co_await m_client.testOrder(std::move(req));
}
