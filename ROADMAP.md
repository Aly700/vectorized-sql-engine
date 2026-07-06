# Roadmap — Vectorized SQL Query Engine

1. Parser, binder, logical algebra, and golden-query oracle.
2. Volcano/interpreted execution for correctness.
3. Columnar storage and vectorized operators.
4. Rule-based rewrites with equivalence tests.
5. Cascades memo, cost model, and join reordering. Delivered in Phase 5c: the memo now explores equivalent join orders, verifies all enumerated alternatives, and extracts a deterministic low-cost logical plan from catalog row-count statistics without changing semantics.
6. ORDER BY end-to-end. Delivered in Phase 6: parser and binder support FROM-scope column sort keys, logical and physical plans include a required-order root `Sort`, both engines implement deterministic stable sorting, and memo verification distinguishes same-plan exact equality from cross-plan ordered bag-plus-sortedness checks.
7. Table aliases and self-joins. Delivered in Phase 7: parser and binder support `FROM t [AS] x` and aliased join chains, bound column identities are binding-scoped, logical/physical scans carry physical table plus binding name, memo scan dedup includes aliases, cost stats look up physical tables while distinct keys remain binding-scoped, and both engines verify aliased self-joins through golden and differential memo alternatives.
8. GROUP BY and aggregate functions. Delivered in Phase 8: parser and binder support `COUNT(*)`, `COUNT(col)`, `SUM`, `MIN`, `MAX`, and `GROUP BY` column refs; `Aggregate` is a logical/physical/memo node between Filter/Join and Project; interpreted and vectorized aggregation preserve first-appearance group order; global empty `COUNT` returns zero while empty `SUM/MIN/MAX` fail loudly in the NULL-free slice; `SUM` detects int64 overflow; join transforms still fire below Aggregate; aggregate queries are covered by golden, binder, cost-model, differential alternatives, and extract_best verification.
9. SELECT output aliases, HAVING, and output-name ORDER BY. Delivered in Phase 9: SELECT-item `AS` aliases define output names under the existing duplicate-output-name invariant; grouped HAVING binds grouping columns, integer literals, and canonical aggregate expressions as a Filter over Aggregate outputs; HAVING-only aggregates are computed internally and dropped by final Project; HAVING without GROUP BY is rejected; ORDER BY resolves exact SELECT output names before falling back to FROM scope, with grouped fallback limited to grouping columns; both engines and the memo/cost model verify the full shape through golden, binder, rewrite, memo, cost, differential alternatives, and extract_best coverage.
10. Predicate pushdown. Delivered in Phase 10: the memo now adds `FilterIntoJoinRule` and `FilterThroughAggregateRule` alternatives without deleting the original shape; one-side WHERE conjuncts can move to join children, both-side conjuncts can merge into Join predicates, and HAVING conjuncts over exact grouping-key identities can move below Aggregate while aggregate-output predicates remain above. Cost-based extraction can now choose lower-cost pushed plans through the existing filter cardinality math, and the differential corpus verifies all alternatives plus `extract_best` for WHERE-over-join and HAVING pushdown stress cases.

## Phase 1 first tasks

- Extend the tiny `SELECT ... FROM ... WHERE col = literal` parser with golden tests.
- Add binder checks for unknown columns and duplicate output names.
- Keep interpreted execution as the oracle before adding more vectorized kernels.
- Add equivalence tests for every rewrite rule.

## Phase discipline

Do not optimize before correctness. Every phase should end with an executable deterministic test or replay artifact.

## Follow-on ideas

- Add global-aggregate HAVING once the NULL-free aggregate contract is extended for that syntax.
- Add sort elimination or sort pushdown only after required physical properties are represented in the memo.
- Add real per-column statistics behind the catalog boundary, such as exact distinct counts or histograms, while preserving deterministic collection.
- Split logical and physical costing once physical join implementations expose build/probe choices explicitly.
- Add an explicit cross-product algebra/property model so association can represent more valid inner-join reorderings without weakening proof comments.
- Add optimizer diagnostics that print group winners, costs, and cardinality estimates for failing golden plan tests.
