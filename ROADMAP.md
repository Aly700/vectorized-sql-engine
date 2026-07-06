# Roadmap — Vectorized SQL Query Engine

1. Parser, binder, logical algebra, and golden-query oracle.
2. Volcano/interpreted execution for correctness.
3. Columnar storage and vectorized operators.
4. Rule-based rewrites with equivalence tests.
5. Cascades memo, cost model, and join reordering. Delivered in Phase 5c: the memo now explores equivalent join orders, verifies all enumerated alternatives, and extracts a deterministic low-cost logical plan from catalog row-count statistics without changing semantics.

## Phase 1 first tasks

- Extend the tiny `SELECT ... FROM ... WHERE col = literal` parser with golden tests.
- Add binder checks for unknown columns and duplicate output names.
- Keep interpreted execution as the oracle before adding more vectorized kernels.
- Add equivalence tests for every rewrite rule.

## Phase discipline

Do not optimize before correctness. Every phase should end with an executable deterministic test or replay artifact.

## Follow-on ideas

- Add real per-column statistics behind the catalog boundary, such as exact distinct counts or histograms, while preserving deterministic collection.
- Split logical and physical costing once physical join implementations expose build/probe choices explicitly.
- Add an explicit cross-product algebra/property model so association can represent more valid inner-join reorderings without weakening proof comments.
- Add optimizer diagnostics that print group winners, costs, and cardinality estimates for failing golden plan tests.
