# Roadmap — Vectorized SQL Query Engine

1. Parser, binder, logical algebra, and golden-query oracle.
2. Volcano/interpreted execution for correctness.
3. Columnar storage and vectorized operators.
4. Rule-based rewrites with equivalence tests.
5. Cascades memo, cost model, and join reordering.

## Phase 1 first tasks

- Extend the tiny `SELECT ... FROM ... WHERE col = literal` parser with golden tests.
- Add binder checks for unknown columns and duplicate output names.
- Keep interpreted execution as the oracle before adding more vectorized kernels.
- Add equivalence tests for every rewrite rule.

## Phase discipline

Do not optimize before correctness. Every phase should end with an executable deterministic test or replay artifact.
