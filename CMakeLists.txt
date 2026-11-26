cmake_minimum_required(VERSION 3.21)
project(matchbook VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MATCHBOOK_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(MATCHBOOK_BUILD_TESTS "Build the unit tests" ON)
option(MATCHBOOK_BUILD_BENCH "Build the benchmark" ON)

# Warnings (and -Werror) apply to our own targets only, not to GoogleTest.
add_library(matchbook_options INTERFACE)
target_compile_options(matchbook_options INTERFACE -Wall -Wextra -Werror)
if(MATCHBOOK_SANITIZE)
  target_compile_options(matchbook_options INTERFACE
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  target_link_options(matchbook_options INTERFACE -fsanitize=address,undefined)
endif()

add_library(matchbook
  src/order_book.cpp src/csv_replay.cpp
  src/feed.cpp src/ledger.cpp src/exchange.cpp src/traffic.cpp src/wire.cpp)
target_include_directories(matchbook PUBLIC include)
target_link_libraries(matchbook PUBLIC matchbook_options)

add_executable(matchbook_cli cli/main.cpp)
target_link_libraries(matchbook_cli PRIVATE matchbook)
set_target_properties(matchbook_cli PROPERTIES OUTPUT_NAME matchbook)

add_executable(matchbook_market cli/market_main.cpp)
target_link_libraries(matchbook_market PRIVATE matchbook)

if(MATCHBOOK_BUILD_BENCH)
  add_executable(matchbook_bench bench/replay_bench.cpp)
  target_link_libraries(matchbook_bench PRIVATE matchbook)
endif()

if(MATCHBOOK_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(matchbook_tests
    tests/order_book_test.cpp
    tests/random_test.cpp
    tests/csv_test.cpp
    tests/exchange_test.cpp
    tests/market_random_test.cpp
    tests/wire_test.cpp)
  target_include_directories(matchbook_tests PRIVATE tests)
  target_link_libraries(matchbook_tests PRIVATE matchbook GTest::gtest_main)
  # Treat GoogleTest headers as system headers so -Werror only applies to our code.
  foreach(tgt gtest gtest_main)
    get_target_property(dirs ${tgt} INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(${tgt} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dirs}")
  endforeach()
  include(GoogleTest)
  gtest_discover_tests(matchbook_tests DISCOVERY_MODE PRE_TEST)
endif()
