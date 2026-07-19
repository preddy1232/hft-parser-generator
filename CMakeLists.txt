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
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ENABLE_NATIVE_ARCH "Optimize for the build machine CPU (non-portable binaries)" OFF)

# Release builds are portable by default so binaries can move between machines.
# Local benchmark builds may opt into CPU-specific instructions explicitly.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -flto")
    if(ENABLE_NATIVE_ARCH)
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=native")
    endif()
elseif(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
endif()

if(ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    else()
        message(FATAL_ERROR "ENABLE_SANITIZERS currently requires GCC or Clang")
    endif()
endif()

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
    # GTest needs RTTI and exceptions — override the global -fno-* flags
    # Also disable LTO so the test binary can link against regular object code
    target_compile_options(gtest PRIVATE -frtti -fexceptions -fno-lto)
    target_compile_options(gtest_main PRIVATE -frtti -fexceptions -fno-lto)
    target_link_options(gtest PRIVATE -fno-lto)
    target_link_options(gtest_main PRIVATE -fno-lto)
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
    set(BENCHMARK_ENABLE_LTO OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)
endif()

# ──────────────────────────────────────────────────────────────────────
# Compiler-Specific Hardened Flags (HFT-grade)
# Note: Placed after FetchContent so third-party deps aren't built with -Werror
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

# ──────────────────────────────────────────────────────────────────────
# Generated Parser Header (header-only library target)
# ──────────────────────────────────────────────────────────────────────
add_library(itch_parser INTERFACE)
target_include_directories(itch_parser INTERFACE
    ${CMAKE_SOURCE_DIR}/generated
    ${CMAKE_SOURCE_DIR}/include
)

add_library(ouch_parser INTERFACE)
target_include_directories(ouch_parser INTERFACE
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
    # Live MoldUDP64 Multicast Ingestion (Phase 3)
    # ──────────────────────────────────────────────────────────────────────
    add_executable(itch_live src/live_main.cpp)
    target_link_libraries(itch_live PRIVATE itch_parser)
    if(WIN32)
        target_link_libraries(itch_live PRIVATE ws2_32)
    endif()

# ──────────────────────────────────────────────────────────────────────
# Unit Tests
# ──────────────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    enable_testing()
    add_executable(itch_tests test/parser_test.cpp)
    # Re-enable exceptions/RTTI for the test binary (GTest requires them)
    # -Wno-conversion: test helpers (write_be16 etc.) do safe masked casts
    # -fno-lto: prevent LTO from mixing RTTI/non-RTTI object files at link time
    target_compile_options(itch_tests PRIVATE
        -fexceptions -frtti -Wno-conversion -fno-lto
    )
    target_link_options(itch_tests PRIVATE -fno-lto)
    target_link_libraries(itch_tests PRIVATE
        itch_parser
        GTest::gtest_main
    )
    include(GoogleTest)
    gtest_discover_tests(itch_tests)

    add_executable(market_tests test/market_test.cpp)
    target_compile_options(market_tests PRIVATE
        -fexceptions -frtti -Wno-conversion -fno-lto
    )
    target_link_options(market_tests PRIVATE -fno-lto)
    target_link_libraries(market_tests PRIVATE
        itch_parser
        GTest::gtest_main
    )
    gtest_discover_tests(market_tests)

    add_executable(ouch_tests test/ouch_test.cpp)
    target_compile_options(ouch_tests PRIVATE
        -fexceptions -frtti -Wno-conversion -fno-lto
    )
    target_link_options(ouch_tests PRIVATE -fno-lto)
    target_link_libraries(ouch_tests PRIVATE
        ouch_parser
        GTest::gtest_main
    )
    gtest_discover_tests(ouch_tests)
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

    add_executable(itch_lob_bench bench/lob_benchmark.cpp)
    target_link_libraries(itch_lob_bench PRIVATE
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

    if(BUILD_TESTS)
        set(SYNTHETIC_ITCH_FIXTURE
            ${CMAKE_BINARY_DIR}/fixtures/synthetic_itch.bin)
        add_custom_command(
            OUTPUT ${SYNTHETIC_ITCH_FIXTURE}
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/generator/generate_synthetic_itch.py
                ${SYNTHETIC_ITCH_FIXTURE}
            DEPENDS
                ${CMAKE_SOURCE_DIR}/generator/generate_synthetic_itch.py
            COMMENT "Generating deterministic synthetic ITCH fixture..."
            VERBATIM
        )
        add_custom_target(generate_synthetic_fixture
            DEPENDS ${SYNTHETIC_ITCH_FIXTURE})

        add_executable(replay_tests test/replay_test.cpp)
        add_dependencies(replay_tests generate_synthetic_fixture)
        target_compile_options(replay_tests PRIVATE
            -fexceptions -frtti -Wno-conversion -fno-lto
        )
        target_link_options(replay_tests PRIVATE -fno-lto)
        target_compile_definitions(replay_tests PRIVATE
            SYNTHETIC_ITCH_FIXTURE="${SYNTHETIC_ITCH_FIXTURE}")
        target_link_libraries(replay_tests PRIVATE
            itch_parser
            GTest::gtest_main
        )
        gtest_discover_tests(replay_tests)
    endif()
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
