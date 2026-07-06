cmake_minimum_required(VERSION 3.20)
project(vectorized_sql_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(sql_engine
    src/sql/parser.cpp
    src/sql/binder.cpp
    src/storage/columnar_batch.cpp
    src/execution/interpreter.cpp
    src/optimizer/memo.cpp
)
target_include_directories(sql_engine PUBLIC include)
target_compile_options(sql_engine PRIVATE -Wall -Wextra -Wpedantic)

add_executable(sql_smoke tests/smoke.cpp)
target_link_libraries(sql_smoke PRIVATE sql_engine)

add_executable(sql_golden tests/golden_queries.cpp)
target_link_libraries(sql_golden PRIVATE sql_engine)

include(CTest)
add_test(NAME sql_smoke COMMAND sql_smoke)
add_test(NAME sql_golden COMMAND sql_golden)
