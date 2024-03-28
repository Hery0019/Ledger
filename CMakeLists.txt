cmake_minimum_required(VERSION 3.20)
project(ledger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_library(ledger_warnings INTERFACE)
target_compile_options(ledger_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror>
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# ---- core -------------------------------------------------------------------
add_library(ledger_core
  src/core/result.cpp
  src/core/value.cpp
)
target_include_directories(ledger_core PUBLIC src)
target_link_libraries(ledger_core PUBLIC ledger_warnings)

# ---- tests ------------------------------------------------------------------
enable_testing()

add_executable(ledger_tests
  tests/main.cpp
  tests/test_result.cpp
  tests/test_value.cpp
)
target_include_directories(ledger_tests PRIVATE third_party)
target_link_libraries(ledger_tests PRIVATE ledger_core)
add_test(NAME ledger_tests COMMAND ledger_tests)
