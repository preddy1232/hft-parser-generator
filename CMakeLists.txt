cmake_minimum_required(VERSION 3.22)

project(hft_parser_generator
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "Zero-Copy NASDAQ ITCH 5.0 Parser Generator for HFT"
)

# ──────────────────────────────────────────────────────────────────────
# Global C++20 Standard
# ──────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ──────────────────────────────────────────────────────────────────────
# Compiler-Specific Hardened Flags (HFT-grade)
# ──────────────────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic -Werror
        -Wconversion -Wshadow -Wnon-virtual-dtor
        -Wold-style-cast -Wcast-align -Wunused
        -Woverloaded-virtual -Wformat=2
        -fno-exceptions      # HFT: exceptions are latency poison
        -fno-rtti            # HFT: RTTI is dead weight on the hot path
    )
endif()

# Release-mode flags: -O3, -march=native, LTO
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto")
endif()

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ──────────────────────────────────────────────────────────────────────
# Options
# ──────────────────────────────────────────────────────────────────────
option(BUILD_TESTS       "Build unit tests (requires GTest)"           ON)
option(BUILD_BENCHMARKS  "Build benchmarks (requires Google Benchmark)" ON)
option(BUILD_GODBOLT     "Build Godbolt-ready snippet for asm analysis" ON)

# ──────────────────────────────────────────────────────────────────────
# FetchContent: Google Test + Google Benchmark
# ──────────────────────────────────────────────────────────────────────
include(FetchContent)

if(BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )
    # Prevent GTest from overriding compiler/linker settings on Windows
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    # GTest uses exceptions internally — allow it for the test target only
    set(gtest_disable_pthreads OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

if(BUILD_BENCHMARKS)
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.4
        GIT_SHALLOW    TRUE
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_EXCEPTIONS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_LTO ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)
endif()

# ──────────────────────────────────────────────────────────────────────
# Generated Parser Header (header-only library target)
# ──────────────────────────────────────────────────────────────────────
add_library(itch_parser INTERFACE)
target_include_directories(itch_parser INTERFACE
    ${CMAKE_SOURCE_DIR}/generated
    ${CMAKE_SOURCE_DIR}/include
)

# ──────────────────────────────────────────────────────────────────────
# Main executable (example / integration driver)
# ──────────────────────────────────────────────────────────────────────
add_executable(itch_driver src/main.cpp)
target_link_libraries(itch_driver PRIVATE itch_parser)

# ──────────────────────────────────────────────────────────────────────
# ITCH File / PCAP Reader CLI tool
# ──────────────────────────────────────────────────────────────────────
add_executable(itch_reader src/itch_reader_main.cpp)
target_link_libraries(itch_reader PRIVATE itch_parser)

# ──────────────────────────────────────────────────────────────────────
# Limit Order Book Engine (Phase 2)
# ──────────────────────────────────────────────────────────────────────
add_executable(itch_lob src/lob_main.cpp)
target_link_libraries(itch_lob PRIVATE itch_parser)

# ──────────────────────────────────────────────────────────────────────
# Unit Tests
# ──────────────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    enable_testing()
    add_executable(itch_tests test/parser_test.cpp)
    # Re-enable exceptions for the test binary only (GTest requires them)
    target_compile_options(itch_tests PRIVATE
        -fexceptions -frtti
    )
    target_link_libraries(itch_tests PRIVATE
        itch_parser
        GTest::gtest_main
    )
    include(GoogleTest)
    gtest_discover_tests(itch_tests)
endif()

# ──────────────────────────────────────────────────────────────────────
# Google Benchmark
# ──────────────────────────────────────────────────────────────────────
if(BUILD_BENCHMARKS)
    add_executable(itch_bench bench/benchmark.cpp)
    target_link_libraries(itch_bench PRIVATE
        itch_parser
        benchmark::benchmark
        benchmark::benchmark_main
    )
endif()

# ──────────────────────────────────────────────────────────────────────
# Godbolt Snippet (minimal, self-contained for assembly analysis)
# ──────────────────────────────────────────────────────────────────────
if(BUILD_GODBOLT)
    add_executable(godbolt_snippet godbolt/godbolt_snippet.cpp)
    target_link_libraries(godbolt_snippet PRIVATE itch_parser)
endif()

# ──────────────────────────────────────────────────────────────────────
# Custom target: run the Python code generator
# ──────────────────────────────────────────────────────────────────────
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_custom_target(generate
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/generator/generator.py
            ${CMAKE_SOURCE_DIR}/schema/itch50_schema.json
            ${CMAKE_SOURCE_DIR}/generated/generated_itch_parser.hpp
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Generating ITCH 5.0 parser from schema..."
        VERBATIM
    )
else()
    message(STATUS "Python3 not found — 'generate' custom target will not be available.")
endif()

# ──────────────────────────────────────────────────────────────────────
# Print Configuration Summary
# ──────────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "╔══════════════════════════════════════════════════════════════╗")
message(STATUS "║  HFT Parser Generator — Build Configuration                ║")
message(STATUS "╠══════════════════════════════════════════════════════════════╣")
message(STATUS "║  Build Type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "║  Compiler       : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "║  C++ Standard   : ${CMAKE_CXX_STANDARD}")
message(STATUS "║  Tests          : ${BUILD_TESTS}")
message(STATUS "║  Benchmarks     : ${BUILD_BENCHMARKS}")
message(STATUS "║  Godbolt        : ${BUILD_GODBOLT}")
message(STATUS "╚══════════════════════════════════════════════════════════════╝")
message(STATUS "")
