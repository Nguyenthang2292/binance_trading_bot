cmake_minimum_required(VERSION 3.16)
project(binance_trading_bot VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    # Avoid C1041 when parallel compilation writes to the same PDB file.
    add_compile_options(
        $<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C>>:/FS>
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/FS>
        $<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/await:strict>
    )
endif()

include(FetchContent)

set(BOOST_INCLUDE_LIBRARIES beast asio system)
FetchContent_Declare(
    Boost
    URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-cmake.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(Boost)

FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v3.10.0
)
FetchContent_MakeAvailable(simdjson)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(OpenSSL REQUIRED)
if(NOT WIN32)
    find_package(SQLite3 REQUIRED)
endif()

set(LIB_SOURCES
    src/context.cpp
    src/types/error.cpp
    src/transport/http_session.cpp
    src/transport/rate_limit_headers.cpp
    src/transport/ws_session.cpp
    src/rest/rest_client.cpp
    src/rest/signer.cpp
    src/rest/rate_limiter.cpp
    src/orders/decimal_string.cpp
    src/orders/order_id_generator.cpp
    src/orders/order_journal.cpp
    src/orders/order_mapper.cpp
    src/orders/order_validator.cpp
    src/orders/order_result.cpp
    src/orders/normal_order_service.cpp
    src/orders/algo_order_service.cpp
    src/orders/mql4_adapter.cpp
    src/orders/orders.cpp
    src/orders/rest_client_adapter.cpp
    src/account/rest_account_client_adapter.cpp
    src/account/mql4_account_adapter.cpp
    src/account/account_service.cpp
    src/ws/ws_client.cpp
    src/ws/user_data_stream.cpp
    src/scanner/kline_cache.cpp
    src/scanner/market_scanner.cpp
    src/strategy/strategy_registry.cpp
    src/strategy/indicators/atr.cpp
    src/engine/position_tracker.cpp
    src/engine/sizing_policy.cpp
    src/engine/work_queue.cpp
    src/engine/beta_calculator.cpp
    src/engine/order_cap_controller.cpp
    src/engine/exposure_controller.cpp
    src/engine/gemini_filter.cpp
    src/engine/loss_manager.cpp
    src/engine/price_filter.cpp
    src/engine/take_profit_reconciler.cpp
    src/engine/trailing_stop_controller.cpp
    src/engine/signal_engine.cpp
    src/engine/qlib_execution_planner.cpp
    src/engine/iexecution_planner.cpp
    src/risk/risk_db.cpp
    src/risk/equity_curve.cpp
    src/risk/risk_metrics.cpp
    src/risk/risk_controller.cpp
    src/orchestration/qlib_state_store.cpp
    src/orchestration/shadow_metrics_recorder.cpp
    src/orchestration/model_publisher.cpp
    src/orchestration/process_manager.cpp
    src/orchestration/batch_scheduler_thread.cpp
    src/orchestration/candle_scheduler_thread.cpp
    src/orchestration/promotion_checker.cpp
    src/catalog/plugin_handle.cpp
    src/catalog/plugin_loader.cpp
    src/catalog/strategy_catalog.cpp
    src/catalog/catalog_reporter.cpp
    src/engine/risk_profile.cpp
    src/binance_api.cpp
    src/trading_engine.cpp
    src/logger.cpp
    src/backtest/parameter_space.cpp
    src/backtest/walk_forward.cpp
    src/backtest/historical_window_provider.cpp
    src/backtest/rest_backfilling_historical_window_provider.cpp
    src/backtest/rest_client_kline_adapter.cpp
    src/backtest/backtest_engine.cpp
    src/backtest/plateau_finder.cpp
    src/backtest/indicator_adapters.cpp
    src/backtest/backtest_gate_controller.cpp
    src/backtest/gemini_range_proposer.cpp
)

set(LIB_HEADERS
    src/context.h
    src/common/expected_compat.h
    src/types/error.h
    src/types/market.h
    src/types/trade.h
    src/types/account.h
    src/types/events.h
    src/transport/http_session.h
    src/transport/rate_limit_headers.h
    src/transport/socks5_proxy.h
    src/transport/ws_session.h
    src/rest/rest_client.h
    src/rest/signer.h
    src/rest/rate_limiter.h
    src/orders/decimal_string.h
    src/orders/order_common.h
    src/orders/order_drafts.h
    src/orders/order_result.h
    src/orders/order_types.h
    src/orders/order_id_generator.h
    src/orders/order_journal.h
    src/orders/order_mapper.h
    src/orders/order_validator.h
    src/orders/normal_order_service.h
    src/orders/algo_order_service.h
    src/orders/mql4_adapter.h
    src/orders/irest_client.h
    src/orders/rest_client_adapter.h
    src/orders/orders.h
    src/account/irest_account_client.h
    src/account/rest_account_client_adapter.h
    src/account/account_snapshot.h
    src/account/mql4_account_adapter.h
    src/account/account_service.h
    src/ws/ws_client.h
    src/ws/user_data_stream.h
    src/scanner/kline_cache.h
    src/scanner/market_scanner.h
    src/strategy/strategy_config.h
    src/strategy/istrategy.h
    src/strategy/strategy_registry.h
    src/strategy/indicators/atr.h
    src/engine/position_tracker.h
    src/engine/sizing_policy.h
    src/engine/work_queue.h
    src/engine/beta_calculator.h
    src/engine/order_cap_controller.h
    src/engine/exposure_controller.h
    src/engine/gemini_filter.h
    src/engine/loss_manager.h
    src/engine/price_filter.h
    src/engine/take_profit_reconciler.h
    src/engine/trailing_stop_controller.h
    src/engine/signal_engine.h
    src/risk/risk_types.h
    src/risk/irisk_port.h
    src/risk/risk_db.h
    src/risk/equity_curve.h
    src/risk/risk_metrics.h
    src/risk/risk_controller.h
    src/orchestration/runtime_ports.h
    src/orchestration/sqlite_helpers.h
    src/orchestration/qlib_state_store.h
    src/orchestration/shadow_metrics_recorder.h
    src/orchestration/model_publisher.h
    src/orchestration/process_manager.h
    src/orchestration/batch_scheduler_thread.h
    src/orchestration/candle_scheduler_thread.h
    src/orchestration/promotion_checker.h
    src/orchestration/orchestrator_config.h
    src/catalog/plugin_handle.h
    src/catalog/plugin_loader.h
    src/catalog/strategy_catalog.h
    src/catalog/catalog_reporter.h
    src/engine/risk_profile.h
    src/binance_api.h
    src/trading_engine.h
    src/logger.h
    src/backtest/backtest_gate.h
    src/backtest/ihistorical_window_provider.h
    src/backtest/ikline_rest_client.h
    src/backtest/parameter_space.h
    src/backtest/walk_forward.h
    src/backtest/historical_window_provider.h
    src/backtest/rest_backfilling_historical_window_provider.h
    src/backtest/rest_client_kline_adapter.h
    src/backtest/backtest_engine.h
    src/backtest/plateau_finder.h
    src/backtest/indicator_adapters.h
    src/backtest/optimizable_strategy.h
    src/backtest/range_proposer.h
    src/backtest/fake_range_proposer.h
    src/backtest/backtest_gate_controller.h
    src/backtest/gemini_range_proposer.h
)

add_library(bot_lib STATIC ${LIB_SOURCES} ${LIB_HEADERS})
add_library(binance_futures ALIAS bot_lib)

target_include_directories(bot_lib PUBLIC src)
target_link_libraries(bot_lib PUBLIC
    Boost::beast
    Boost::asio
    Boost::system
    simdjson::simdjson
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
)
if(NOT WIN32)
    target_link_libraries(bot_lib PUBLIC SQLite::SQLite3)
    target_link_libraries(bot_lib PUBLIC pthread)
endif()
if(WIN32)
    target_link_libraries(bot_lib PUBLIC Crypt32 winsqlite3)
endif()
target_compile_features(bot_lib PUBLIC cxx_std_23)
target_compile_options(bot_lib PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
target_compile_definitions(bot_lib PUBLIC $<$<CXX_COMPILER_ID:MSVC>:_WIN32_WINNT=0x0A00>)

add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE bot_lib)
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_subdirectory(plugins/src/trend_breakout)
add_subdirectory(plugins/src/gartley_day_crossover)
add_subdirectory(plugins/src/donchian_5_20_crossover)
add_subdirectory(plugins/src/golden_crossover)
add_subdirectory(plugins/src/monthly_close_model)
add_subdirectory(plugins/src/ricardo_rules)
add_subdirectory(plugins/src/dow_theory)
add_subdirectory(plugins/src/qlib_model_signal)
add_subdirectory(plugins/src/qlib_strategy_signal)

install(TARGETS ${PROJECT_NAME} DESTINATION bin)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()

file(GLOB TEST_SOURCES tests/*.cpp)

add_executable(${PROJECT_NAME}_tests ${TEST_SOURCES})
target_link_libraries(${PROJECT_NAME}_tests PRIVATE
    bot_lib
    GTest::gtest_main
)
add_dependencies(${PROJECT_NAME}_tests
    strategy_trend_breakout
    strategy_gartley_day_crossover
    strategy_donchian_5_20_crossover
    strategy_golden_crossover
    strategy_monthly_close_model
    strategy_ricardo_rules
    strategy_dow_theory
    strategy_qlib_model_signal
    strategy_qlib_strategy_signal
)
target_include_directories(${PROJECT_NAME}_tests PRIVATE src)
target_compile_definitions(${PROJECT_NAME}_tests PRIVATE BOT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(${PROJECT_NAME}_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

include(GoogleTest)
gtest_discover_tests(${PROJECT_NAME}_tests)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import pytest"
        RESULT_VARIABLE PYTEST_AVAILABLE
    )
    if(PYTEST_AVAILABLE EQUAL 0)
        add_test(
            NAME python_gemini_filter_pytests
            COMMAND ${Python3_EXECUTABLE} -m pytest -q ${CMAKE_SOURCE_DIR}/tests/python
        )
        add_test(
            NAME python_backtest_range_proposer_pytests
            COMMAND ${Python3_EXECUTABLE} -m pytest -q ${CMAKE_SOURCE_DIR}/tests/python/test_backtest_range_proposer.py
        )
    else()
        message(WARNING "pytest is not available; skipping python_gemini_filter_pytests")
    endif()
endif()
