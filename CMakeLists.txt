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
    src/engine/exposure_controller.cpp
    src/engine/trailing_stop_controller.cpp
    src/engine/signal_engine.cpp
    src/catalog/plugin_handle.cpp
    src/catalog/plugin_loader.cpp
    src/catalog/strategy_catalog.cpp
    src/catalog/catalog_reporter.cpp
    src/binance_api.cpp
    src/trading_engine.cpp
    src/logger.cpp
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
    src/engine/exposure_controller.h
    src/engine/trailing_stop_controller.h
    src/engine/signal_engine.h
    src/catalog/plugin_handle.h
    src/catalog/plugin_loader.h
    src/catalog/strategy_catalog.h
    src/catalog/catalog_reporter.h
    src/binance_api.h
    src/trading_engine.h
    src/logger.h
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
    target_link_libraries(bot_lib PUBLIC pthread)
endif()
target_compile_features(bot_lib PUBLIC cxx_std_23)
target_compile_options(bot_lib PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
target_compile_definitions(bot_lib PUBLIC $<$<CXX_COMPILER_ID:MSVC>:_WIN32_WINNT=0x0A00>)

add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE bot_lib)
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

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
target_include_directories(${PROJECT_NAME}_tests PRIVATE src)
set_target_properties(${PROJECT_NAME}_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

include(GoogleTest)
gtest_discover_tests(${PROJECT_NAME}_tests)
