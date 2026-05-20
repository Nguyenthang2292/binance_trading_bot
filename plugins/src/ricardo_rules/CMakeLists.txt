cmake_minimum_required(VERSION 3.16)
project(strategy_ricardo_rules LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(strategy_ricardo_rules SHARED
    strategy_ricardo_rules.cpp
    ../../../src/strategy/indicators/atr.cpp
)

target_include_directories(strategy_ricardo_rules PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)

target_link_libraries(strategy_ricardo_rules PRIVATE
    nlohmann_json::nlohmann_json
)

set(SAFE_COPY_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/safe_copy_if_different.cmake")
add_custom_command(TARGET strategy_ricardo_rules POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        -Dsrc="$<TARGET_FILE:strategy_ricardo_rules>"
        -Ddst="$<TARGET_FILE_DIR:binance_trading_bot>/plugins/$<TARGET_FILE_NAME:strategy_ricardo_rules>"
        -P "${SAFE_COPY_SCRIPT}"
    COMMAND "${CMAKE_COMMAND}"
        -Dsrc="$<TARGET_FILE:strategy_ricardo_rules>"
        -Ddst="${CMAKE_SOURCE_DIR}/plugins/$<TARGET_FILE_NAME:strategy_ricardo_rules>"
        -P "${SAFE_COPY_SCRIPT}"
)
