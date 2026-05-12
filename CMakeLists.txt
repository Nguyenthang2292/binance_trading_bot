cmake_minimum_required(VERSION 3.16)
project(binance_trading_bot VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

find_package(OpenSSL REQUIRED)

set(LIB_SOURCES
    src/context.cpp
    src/types/error.cpp
    src/transport/http_session.cpp
    src/transport/ws_session.cpp
    src/rest/rest_client.cpp
    src/rest/signer.cpp
    src/rest/rate_limiter.cpp
    src/orders/decimal_string.cpp
    src/orders/order_id_generator.cpp
    src/orders/order_journal.cpp
    src/orders/order_mapper.cpp
    src/orders/order_validator.cpp
    src/orders/orders.cpp
    src/orders/rest_client_adapter.cpp
    src/ws/ws_client.cpp
    src/ws/user_data_stream.cpp
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
    src/transport/ws_session.h
    src/rest/rest_client.h
    src/rest/signer.h
    src/rest/rate_limiter.h
    src/orders/decimal_string.h
    src/orders/order_types.h
    src/orders/order_id_generator.h
    src/orders/order_journal.h
    src/orders/order_mapper.h
    src/orders/order_validator.h
    src/orders/irest_client.h
    src/orders/rest_client_adapter.h
    src/orders/orders.h
    src/ws/ws_client.h
    src/ws/user_data_stream.h
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
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)
target_compile_features(bot_lib PUBLIC cxx_std_23)

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
