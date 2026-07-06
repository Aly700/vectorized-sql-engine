# Architecture — Vectorized SQL Query Engine

## Pipeline

```text
SQL text -> parser -> binder/type checker -> logical plan -> optimizer memo -> physical plan -> vectorized execution
```

## Current scaffold

- `sql::parse_select` implements a small, explicit SQL slice for golden tests and rejects unsupported syntax with position-bearing parse errors. The slice includes `SELECT ... FROM t1 [INNER] JOIN t2 ON ...` chains with conjunction-only `ON` and `WHERE` comparisons.
- `catalog::Catalog` is the neutral schema boundary between SQL binding and execution. It exposes table names, column names, and column types without table data.
- `sql::bind_select` resolves parsed column references against catalog table schema scopes and rejects binding errors before logical planning. Qualified refs bind only to the named table scope. Unqualified refs must match exactly one visible table scope. The sql layer does not include execution headers.
- `plan::LogicalPlan` is the post-binding algebra handoff boundary. Expressions in logical plans carry bound column identities (`table`, `column`) so downstream layers never re-resolve parsed SQL names.
- `optimizer::rewrite_to_fixpoint` applies a deterministic ordered list of pure `LogicalPlan -> optional<LogicalPlan>` rewrite rules before physical lowering.
- `plan::PhysicalPlan` is a structure-preserving lowering of the logical tree. It does not optimize, cost, or reorder.
- `execution::execute_interpreted` is the correctness oracle, including inner join semantics.
- `execution::execute_vectorized` lowers to the physical tree and runs scan/filter/project with immutable selection vectors until final materialization. Inner join is intentionally unsupported in the vectorized engine until the later vectorized join phase; attempts to run a join there fail with a clear not-yet-supported error.
- `storage::ColumnarBatch` enforces equal-length column vectors.
- `optimizer::Memo` is the shell for Cascades groups.
- `execution::Catalog` implements `catalog::Catalog` for binding and separately owns table batches for execution.

## Rule-based rewrites

Rules live beside, but not inside, `optimizer::Memo` so they can later be re-hosted onto memo groups. Each rule is a pure transform from a `plan::LogicalPlan` to either an equivalent replacement plan or `std::nullopt`.

The driver traverses child-first, tries rules in vector order, applies at most one rewrite per pass, records the fired rule name, and repeats to a fixpoint with a hard max-pass bound. For the shipped default rules, termination is monotonic: constant folding removes non-canonical literal comparisons by replacing them with canonical booleans; adjacent-filter merge removes a filter node while preserving predicate order; the always-false rule collapses a predicate list to one canonical false predicate; and the always-true rule removes predicates or an entire filter. No default rule reintroduces a non-canonical literal comparison, adjacent filter pair, canonical true predicate, or larger predicate list after simplification, so the bounded fixpoint is a guard against future cyclic rules rather than part of the proof.

Rewrite equivalence is tested by running each corpus query three ways: unrewritten logical plan through the interpreted oracle, rewritten logical plan through the interpreted oracle, and rewritten logical plan through the vectorized engine. This protects the invariants that rewrites preserve bag semantics and that vectorized execution matches the oracle for the same logical plan.

Join queries are included in the rewrite corpus through the oracle paths only until vectorized join exists. For these marked cases, the corpus still compares unrewritten and rewritten interpreted results, then verifies that vectorized execution reports inner join as not yet supported rather than pretending to implement it.

## Inner joins

The frontend supports inner join chains without aliases:

```text
SELECT select_list
FROM t1 [INNER] JOIN t2 ON comparison [AND comparison]*
        [JOIN t3 ON comparison [AND comparison]* ...]
[WHERE comparison [AND comparison]*]
```

Join chains bind as left-deep logical plans. `Join` predicates live on the `Join` node. A join node's output identity and order are deterministic: all left child columns, followed by all right child columns. Scans materialize internal identities as `table.column`; projection output names are separate user-visible names.

Qualified projected columns use their qualified spelling as the output name, so `SELECT t1.a, t2.a ...` produces output columns `t1.a` and `t2.a`. Unqualified projected columns use the bare column name, so duplicate output names are still rejected by name.

The interpreted oracle implements nested-loop inner join with bag semantics. The row order is part of the SQL engine contract: for each left row in input order, visit each right row in input order, emitting every pair whose `ON` conjuncts are true. Duplicate matches multiply, and an empty side yields zero output rows with the deterministic joined column order. Future vectorized join work must reproduce this left-row-major order exactly.

## Vectorized execution

Scan creates an identity selection vector in table row order. Filter evaluates predicates over the current selection vector and returns a newly allocated `shared_ptr<const vector<size_t>>`; downstream operators cannot mutate a handed-off selection. Project is the materialization boundary for the supported slice: it walks the selected row ids in order, evaluates scalar expressions, and builds output columns through `ColumnarBatch::add_column`, preserving equal row counts and SELECT-list order.

## Design bias

The vectorized engine should never outrun the oracle. Add interpreted semantics first, then prove vectorized operators match it.
