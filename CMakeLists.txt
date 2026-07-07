cmake_minimum_required(VERSION 3.20)
project(vectorized_sql_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(sql_engine
    src/sql/parser.cpp
    src/sql/binder.cpp
    src/plan/logical_plan.cpp
    src/plan/physical_plan.cpp
    src/storage/columnar_batch.cpp
    src/execution/interpreter.cpp
    src/execution/vectorized.cpp
    src/optimizer/explain.cpp
    src/optimizer/memo.cpp
    src/optimizer/rewrite.cpp
)
target_include_directories(sql_engine PUBLIC include)
target_compile_options(sql_engine PRIVATE -Wall -Wextra -Wpedantic)

add_executable(sql_smoke tests/smoke.cpp)
target_link_libraries(sql_smoke PRIVATE sql_engine)

add_executable(sql_golden tests/golden_queries.cpp)
target_link_libraries(sql_golden PRIVATE sql_engine)

add_executable(sql_vectorized_differential tests/vectorized_differential.cpp)
target_link_libraries(sql_vectorized_differential PRIVATE sql_engine)

add_executable(sql_fuzz_differential tests/sql_fuzz_differential.cpp)
target_link_libraries(sql_fuzz_differential PRIVATE sql_engine)

add_executable(sql_catalog_binder tests/catalog_binder.cpp)
target_link_libraries(sql_catalog_binder PRIVATE sql_engine)

add_executable(sql_rewrite_rules tests/rewrite_rules.cpp)
target_link_libraries(sql_rewrite_rules PRIVATE sql_engine)

add_executable(sql_optimizer_memo tests/memo.cpp)
target_link_libraries(sql_optimizer_memo PRIVATE sql_engine)

add_executable(sql_optimizer_cost_model tests/cost_model.cpp)
target_link_libraries(sql_optimizer_cost_model PRIVATE sql_engine)

add_executable(sql_bench benchmarks/sql_bench.cpp)
target_link_libraries(sql_bench PRIVATE sql_engine)

include(CTest)
add_test(NAME sql_smoke COMMAND sql_smoke)
add_test(NAME sql_golden COMMAND sql_golden)
add_test(NAME sql_vectorized_differential COMMAND sql_vectorized_differential)
add_test(NAME sql_fuzz_differential COMMAND sql_fuzz_differential)
add_test(NAME sql_catalog_binder COMMAND sql_catalog_binder)
add_test(NAME sql_rewrite_rules COMMAND sql_rewrite_rules)
add_test(NAME sql_optimizer_memo COMMAND sql_optimizer_memo)
add_test(NAME sql_optimizer_cost_model COMMAND sql_optimizer_cost_model)
