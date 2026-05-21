cmake_minimum_required(VERSION 3.16)
project(strategy_qlib_model_signal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(strategy_qlib_model_signal SHARED
    strategy_qlib_model_signal.cpp
)

target_include_directories(strategy_qlib_model_signal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)

target_link_libraries(strategy_qlib_model_signal PRIVATE
    nlohmann_json::nlohmann_json
)

if(WIN32)
    target_link_libraries(strategy_qlib_model_signal PRIVATE winsqlite3)
else()
    find_package(SQLite3 REQUIRED)
    target_link_libraries(strategy_qlib_model_signal PRIVATE SQLite::SQLite3)
endif()

set(SAFE_COPY_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/safe_copy_if_different.cmake")
add_custom_command(TARGET strategy_qlib_model_signal POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        -Dsrc="$<TARGET_FILE:strategy_qlib_model_signal>"
        -Ddst="$<TARGET_FILE_DIR:binance_trading_bot>/plugins/$<TARGET_FILE_NAME:strategy_qlib_model_signal>"
        -P "${SAFE_COPY_SCRIPT}"
)
