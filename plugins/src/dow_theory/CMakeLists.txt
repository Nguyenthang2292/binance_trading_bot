cmake_minimum_required(VERSION 3.16)
project(strategy_dow_theory LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(strategy_dow_theory SHARED
    strategy_dow_theory.cpp
    ../../../src/strategy/indicators/atr.cpp
)

target_include_directories(strategy_dow_theory PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)

target_link_libraries(strategy_dow_theory PRIVATE
    nlohmann_json::nlohmann_json
)
