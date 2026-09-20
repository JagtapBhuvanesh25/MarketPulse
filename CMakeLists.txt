cmake_minimum_required(VERSION 3.28)
project(MarketPulse LANGUAGES CXX)

# ============================================================
# C++20, strict warnings (warnings are errors per spec)
# ============================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Strict warning flags — applied to all our targets (NOT to fetched deps)
set(MP_WARN_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    # Additional useful warnings
    -Wshadow
    -Wnull-dereference
    -Wno-error=pedantic
    -Wno-error=sign-conversion
    -Wno-error=conversion
    -Wno-unused-result
    -Wno-error=unused-result
    -Wno-error=misleading-indentation
    -Wno-error=unused-variable
)

# Sanitizer support: -DSANITIZE=address|thread|undefined
if(DEFINED SANITIZE)
    if(SANITIZE STREQUAL "address")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    elseif(SANITIZE STREQUAL "thread")
        add_compile_options(-fsanitize=thread)
        add_link_options(-fsanitize=thread)
    elseif(SANITIZE STREQUAL "undefined")
        add_compile_options(-fsanitize=undefined)
        add_link_options(-fsanitize=undefined)
    endif()
    message(STATUS "Sanitizer: ${SANITIZE}")
endif()

# ============================================================
# Dependencies via FetchContent (pinned to specific tags)
# ============================================================
include(FetchContent)

find_package(OpenSSL REQUIRED)
set(USE_TLS ON CACHE BOOL "Enable TLS in IXWebSocket" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "Use OpenSSL for IXWebSocket TLS" FORCE)

# 1. IXWebSocket — WebSocket client/server + HTTP
FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        v11.4.4
)

# 2. simdjson — fast JSON parsing, on-demand API
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        v3.10.1
)

# 3. HdrHistogram_c — latency percentile recording
FetchContent_Declare(
    hdrhistogram
    GIT_REPOSITORY https://github.com/HdrHistogram/HdrHistogram_c.git
    GIT_TAG        0.11.8
)

# 4. Catch2 v3 — unit tests
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
)

# Suppress warnings in fetched dependencies
set(CMAKE_CXX_FLAGS_BEFORE "${CMAKE_CXX_FLAGS}")

FetchContent_MakeAvailable(ixwebsocket simdjson hdrhistogram Catch2)

# ============================================================
# MarketPulse library (all shared sources)
# ============================================================
add_library(marketpulse_lib STATIC
    src/sequencer.cpp
    src/book_map.cpp
    src/book_ladder.cpp
    src/signals.cpp
    src/latency.cpp
    src/sim.cpp
    src/broadcast.cpp
    src/feed_binance.cpp
)

# Backward-compatibility alias
add_library(obl_lib ALIAS marketpulse_lib)
add_library(mp_lib ALIAS marketpulse_lib)

target_include_directories(marketpulse_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(marketpulse_lib PUBLIC
    ixwebsocket
    simdjson
    hdr_histogram_static
    pthread
    OpenSSL::SSL
    OpenSSL::Crypto
)

target_compile_options(marketpulse_lib PRIVATE ${MP_WARN_FLAGS})

# ============================================================
# Main binary: marketpulse
# ============================================================
add_executable(marketpulse src/main.cpp)
target_link_libraries(marketpulse PRIVATE marketpulse_lib)
target_compile_options(marketpulse PRIVATE ${MP_WARN_FLAGS})

# Backward-compatibility aliases
add_executable(obl ALIAS marketpulse)
add_executable(mp ALIAS marketpulse)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(marketpulse PRIVATE -O3)
endif()

# ============================================================
# Tools
# ============================================================
add_executable(capture tools/capture.cpp)
target_link_libraries(capture PRIVATE marketpulse_lib)
target_include_directories(capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(capture PRIVATE ${MP_WARN_FLAGS})

add_executable(replay tools/replay.cpp)
target_link_libraries(replay PRIVATE marketpulse_lib)
target_include_directories(replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(replay PRIVATE ${MP_WARN_FLAGS})

# ============================================================
# Tests (Catch2 v3)
# ============================================================
include(CTest)
include(Catch)

add_executable(test_spsc            tests/test_spsc.cpp)
add_executable(test_sequencer       tests/test_sequencer.cpp)
add_executable(test_book_equivalence tests/test_book_equivalence.cpp)
add_executable(test_signals         tests/test_signals.cpp)

foreach(test_target
        test_spsc
        test_sequencer
        test_book_equivalence
        test_signals)
    target_link_libraries(${test_target} PRIVATE marketpulse_lib Catch2::Catch2WithMain)
    target_compile_options(${test_target} PRIVATE ${MP_WARN_FLAGS})
    catch_discover_tests(${test_target} WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endforeach()

# ============================================================
# Benchmark performance gate
# Generates perf_results.txt on each --bench run.
# CI uses this to compare against perf_baseline.json.
# ============================================================
add_custom_target(bench
    COMMAND $<TARGET_FILE:marketpulse> --bench ${CMAKE_SOURCE_DIR}/tests/fixtures/btcusdt_clean.jsonl
    DEPENDS marketpulse
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running benchmark against btcusdt_clean.jsonl"
)
