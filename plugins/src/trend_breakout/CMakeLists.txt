cmake_minimum_required(VERSION 3.16)
project(strategy_trend_breakout LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(strategy_trend_breakout SHARED
    strategy_trend_breakout.cpp
    ../../../src/strategy/indicators/atr.cpp
)

target_include_directories(strategy_trend_breakout PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)

target_link_libraries(strategy_trend_breakout PRIVATE
    nlohmann_json::nlohmann_json
)

set(PLUGIN_DEPLOY_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
add_custom_command(TARGET strategy_trend_breakout POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PLUGIN_DEPLOY_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:strategy_trend_breakout>"
        "${PLUGIN_DEPLOY_DIR}/$<TARGET_FILE_NAME:strategy_trend_breakout>"
)
