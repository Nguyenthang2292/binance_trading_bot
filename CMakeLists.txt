cmake_minimum_required(VERSION 3.16)
project(binance_trading_bot VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

set(LIB_SOURCES
    src/binance_api.cpp
    src/trading_engine.cpp
    src/logger.cpp
)

set(LIB_HEADERS
    src/binance_api.h
    src/trading_engine.h
    src/logger.h
)

add_library(bot_lib STATIC ${LIB_SOURCES} ${LIB_HEADERS})
target_include_directories(bot_lib PUBLIC src)
target_link_libraries(bot_lib PUBLIC
    CURL::libcurl
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)

add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE bot_lib)
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

install(TARGETS ${PROJECT_NAME} DESTINATION bin)

# ---- Tests ----
include(FetchContent)
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
