# Vectorized SQL Query Engine

[![CI](https://github.com/Aly700/vectorized-sql-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/Aly700/vectorized-sql-engine/actions/workflows/ci.yml)

## Scope

A columnar analytical engine whose hardest boundary is the optimizer: SQL is parsed and bound into relational algebra, rules produce equivalent plans, and a Cascades-style memo chooses a low-cost vectorized physical plan without changing semantics.

## Stack

C++20, CMake, Arrow-like in-memory column batches, SIMD-friendly operator interfaces without external dependencies yet.

## Build and smoke test

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Linux CI runs the full test suite with address and undefined-behavior sanitizers.
Continuous parser and planner fuzzing replays known queries before a timed fuzzing run.

`yard_export` writes `docs/yard/yard-routes.json`: every memo alternative for four workings, priced by the cost model and timed through vectorized execution. See the "Yard Routes" section of `docs/benchmarks.md`.

## Phase map

1. Parser, binder, logical algebra, and golden-query oracle.
2. Volcano/interpreted execution for correctness.
3. Columnar storage and vectorized operators.
4. Rule-based rewrites with equivalence tests.
5. Cascades memo, cost model, and join reordering.
