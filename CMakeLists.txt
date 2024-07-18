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

# ---- sql --------------------------------------------------------------------
add_library(ledger_sql
  src/sql/ast.cpp
  src/sql/lexer.cpp
  src/sql/parser.cpp
)
target_link_libraries(ledger_sql PUBLIC ledger_core)

# ---- semantic ---------------------------------------------------------------
add_library(ledger_semantic
  src/semantic/binder.cpp
  src/semantic/catalog.cpp
  src/semantic/eval.cpp
)
target_link_libraries(ledger_semantic PUBLIC ledger_sql)

# ---- storage ----------------------------------------------------------------
add_library(ledger_storage
  src/storage/codec.cpp
  src/storage/file_engine.cpp
  src/storage/memory_engine.cpp
)
target_link_libraries(ledger_storage PUBLIC ledger_core)

# ---- exec -------------------------------------------------------------------
add_library(ledger_exec
  src/exec/executor.cpp
)
target_link_libraries(ledger_exec PUBLIC ledger_semantic ledger_storage)

# ---- cli --------------------------------------------------------------------
add_library(ledger_cli
  src/cli/database.cpp
)
target_link_libraries(ledger_cli PUBLIC ledger_exec)

add_executable(ledger src/cli/main.cpp)
target_link_libraries(ledger PRIVATE ledger_cli)

# ---- tests ------------------------------------------------------------------
enable_testing()

add_executable(ledger_tests
  tests/main.cpp
  tests/test_result.cpp
  tests/test_value.cpp
  tests/test_lexer.cpp
  tests/test_parser.cpp
  tests/test_catalog.cpp
  tests/test_eval.cpp
  tests/test_binder.cpp
  tests/test_codec.cpp
  tests/test_memory_engine.cpp
  tests/test_file_engine.cpp
  tests/test_executor.cpp
  tests/test_cli.cpp
)
target_include_directories(ledger_tests PRIVATE third_party)
target_link_libraries(ledger_tests PRIVATE ledger_core ledger_sql ledger_semantic ledger_storage ledger_exec ledger_cli)
add_test(NAME ledger_tests COMMAND ledger_tests)
