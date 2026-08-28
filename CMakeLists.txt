cmake_minimum_required(VERSION 3.20)
project(sqltxt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_library(sqltxt_warnings INTERFACE)
target_compile_options(sqltxt_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror>
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# ---- core -------------------------------------------------------------------
add_library(sqltxt_core
  src/core/result.cpp
  src/core/value.cpp
)
target_include_directories(sqltxt_core PUBLIC src)
target_link_libraries(sqltxt_core PUBLIC sqltxt_warnings)

# ---- tests ------------------------------------------------------------------
enable_testing()

add_executable(sqltxt_tests
  tests/main.cpp
  tests/test_result.cpp
  tests/test_value.cpp
)
target_include_directories(sqltxt_tests PRIVATE third_party)
target_link_libraries(sqltxt_tests PRIVATE sqltxt_core)
add_test(NAME sqltxt_tests COMMAND sqltxt_tests)
