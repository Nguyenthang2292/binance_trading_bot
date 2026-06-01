#pragma once

#include "engine/iexecution_planner.h"

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <exception>
#include <string>
#include <type_traits>
#include <utility>

namespace engine {

class QlibExecutionPlanner : public IExecutionPlanner {
public:
    explicit QlibExecutionPlanner(const std::string& dbPath);
    QlibExecutionPlanner(const std::string& dbPath, IExecutionPlanner& nativeFallback);
    ~QlibExecutionPlanner() override;

    boost::asio::awaitable<OrdersResult<NormalPlacementResult>> executeMarket(MarketOrderDraft draft) override;

private:
    // WR-2: run a blocking SQLite operation on the dedicated single-thread DB
    // pool and resume on the caller's executor, so synchronous SQLite (with its
    // multi-second busy_timeout) never blocks an io_context worker thread and
    // cannot starve the shared WS/order coroutines. `fn` is invoked on the pool
    // thread; any exception it throws is captured and re-thrown to the awaiter so
    // the existing fail-closed try/catch in executeMarket still applies.
    template <class Fn>
    boost::asio::awaitable<std::invoke_result_t<Fn>> runOnDbPool(Fn fn) {
        using R = std::invoke_result_t<Fn>;
        struct Outcome {
            R value{};
            std::exception_ptr exception;
        };
        auto token = boost::asio::use_awaitable;
        auto outcome = co_await boost::asio::async_initiate<decltype(token), void(Outcome)>(
            [this, fn = std::move(fn)](auto handler) mutable {
                auto completionExecutor =
                    boost::asio::get_associated_executor(handler, m_dbPool.get_executor());
                boost::asio::post(
                    m_dbPool,
                    [fn = std::move(fn), handler = std::move(handler), completionExecutor]() mutable {
                        Outcome result;
                        try {
                            result.value = fn();
                        } catch (...) {
                            result.exception = std::current_exception();
                        }
                        boost::asio::post(
                            completionExecutor,
                            [result = std::move(result), handler = std::move(handler)]() mutable {
                                handler(std::move(result));
                            });
                    });
            },
            token);
        if (outcome.exception) {
            std::rethrow_exception(outcome.exception);
        }
        co_return std::move(outcome.value);
    }

    std::string m_dbPath;
    IExecutionPlanner* m_nativeFallback{nullptr};
    // Dedicated single worker so all SQLite access on one connection stays
    // serialized (sqlite3 connections are not safe for concurrent use) while
    // remaining off the io_context threads.
    boost::asio::thread_pool m_dbPool{1};
};

} // namespace engine
