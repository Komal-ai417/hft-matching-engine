cmake_minimum_required(VERSION 3.14)
project(HFT_MatchingEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# HFT Optimization flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -flto")
elseif(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /O2 /Oi /GL")
endif()

# Find GoogleTest
find_package(GTest REQUIRED)

# Find GoogleBenchmark
find_package(benchmark REQUIRED)

# Main Executable
add_executable(hft_engine src/main.cpp src/OrderBook.cpp)

# Tests executable
add_executable(hft_tests tests/test_ob.cpp src/OrderBook.cpp)
target_link_libraries(hft_tests GTest::gtest_main)

# Benchmark executable
add_executable(hft_bench tests/benchmark_ob.cpp src/OrderBook.cpp)
target_link_libraries(hft_bench benchmark::benchmark)
