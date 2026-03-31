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
  src/core/password.cpp
  src/core/result.cpp
  src/core/sha256.cpp
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
  src/semantic/bound.cpp
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

# ---- server -----------------------------------------------------------------
add_library(ledger_server
  src/server/handler.cpp
  src/server/json.cpp
)
target_link_libraries(ledger_server PUBLIC ledger_cli)

# cpp-httplib (used through src/server/http.h): a system include so the
# project's -Werror does not apply to the vendored header; sockets need
# threads everywhere and winsock on Windows.
find_package(Threads REQUIRED)
add_library(ledger_httplib INTERFACE)
target_include_directories(ledger_httplib SYSTEM INTERFACE third_party)
target_link_libraries(ledger_httplib INTERFACE Threads::Threads $<$<PLATFORM_ID:Windows>:ws2_32>)

add_executable(ledgerd src/server/main.cpp)
target_link_libraries(ledgerd PRIVATE ledger_server ledger_httplib)
target_link_options(ledgerd PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-static-libstdc++ -static-libgcc>
  $<$<AND:$<CXX_COMPILER_ID:GNU>,$<PLATFORM_ID:Windows>>:-static>
)

add_executable(ledger src/cli/main.cpp)
target_link_libraries(ledger PRIVATE ledger_cli)
# C++ runtime linked statically: the executable must run outside the
# MinGW environment (otherwise libstdc++-6.dll is not found from PowerShell).
target_link_options(ledger PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-static-libstdc++ -static-libgcc>
  $<$<AND:$<CXX_COMPILER_ID:GNU>,$<PLATFORM_ID:Windows>>:-static>
)

# ---- tests ------------------------------------------------------------------
enable_testing()

add_executable(ledger_tests
  tests/main.cpp
  tests/test_result.cpp
  tests/test_value.cpp
  tests/test_lexer.cpp
  tests/test_parser.cpp
  tests/test_password.cpp
  tests/test_catalog.cpp
  tests/test_eval.cpp
  tests/test_binder.cpp
  tests/test_codec.cpp
  tests/test_memory_engine.cpp
  tests/test_file_engine.cpp
  tests/test_executor.cpp
  tests/test_cli.cpp
  tests/test_views.cpp
  tests/test_aggregates.cpp
  tests/test_joins.cpp
  tests/test_predicates.cpp
  tests/test_functions.cpp
  tests/test_subqueries.cpp
  tests/test_transactions.cpp
  tests/test_index.cpp
  tests/test_constraints.cpp
  tests/test_server_handler.cpp
  tests/test_server_http.cpp
  tests/test_server_json.cpp
  tests/test_users.cpp
)
# third_party (doctest, httplib) comes as a system include via ledger_httplib.
target_link_libraries(ledger_tests PRIVATE ledger_core ledger_sql ledger_semantic ledger_storage ledger_exec ledger_cli ledger_server ledger_httplib)
add_test(NAME ledger_tests COMMAND ledger_tests)
