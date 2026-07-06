# Architecture — Vectorized SQL Query Engine

## Pipeline

```text
SQL text -> parser -> binder/type checker -> logical plan -> optimizer memo -> physical plan -> vectorized execution
```

## Current scaffold

- `sql::parse_select` implements a small, explicit SQL slice for golden tests and rejects unsupported syntax with position-bearing parse errors.
- `catalog::Catalog` is the neutral schema boundary between SQL binding and execution. It exposes table names, column names, and column types without table data.
- `sql::bind_select` resolves parsed column references against one catalog table schema scope and rejects binding errors before logical planning. The sql layer does not include execution headers.
- `plan::LogicalPlan` is the post-binding algebra handoff boundary.
- `optimizer::rewrite_to_fixpoint` applies a deterministic ordered list of pure `LogicalPlan -> optional<LogicalPlan>` rewrite rules before physical lowering.
- `plan::PhysicalPlan` is a structure-preserving lowering of the logical tree. It does not optimize, cost, or reorder.
- `execution::execute_interpreted` is the correctness oracle.
- `execution::execute_vectorized` lowers to the physical tree and runs scan/filter/project with immutable selection vectors until final materialization.
- `storage::ColumnarBatch` enforces equal-length column vectors.
- `optimizer::Memo` is the shell for Cascades groups.
- `execution::Catalog` implements `catalog::Catalog` for binding and separately owns table batches for execution.

## Rule-based rewrites

Rules live beside, but not inside, `optimizer::Memo` so they can later be re-hosted onto memo groups. Each rule is a pure transform from a `plan::LogicalPlan` to either an equivalent replacement plan or `std::nullopt`.

The driver traverses child-first, tries rules in vector order, applies at most one rewrite per pass, records the fired rule name, and repeats to a fixpoint with a hard max-pass bound. For the shipped default rules, termination is monotonic: constant folding removes non-canonical literal comparisons by replacing them with canonical booleans; adjacent-filter merge removes a filter node while preserving predicate order; the always-false rule collapses a predicate list to one canonical false predicate; and the always-true rule removes predicates or an entire filter. No default rule reintroduces a non-canonical literal comparison, adjacent filter pair, canonical true predicate, or larger predicate list after simplification, so the bounded fixpoint is a guard against future cyclic rules rather than part of the proof.

Rewrite equivalence is tested by running each corpus query three ways: unrewritten logical plan through the interpreted oracle, rewritten logical plan through the interpreted oracle, and rewritten logical plan through the vectorized engine. This protects the invariants that rewrites preserve bag semantics and that vectorized execution matches the oracle for the same logical plan.

## Vectorized execution

Scan creates an identity selection vector in table row order. Filter evaluates predicates over the current selection vector and returns a newly allocated `shared_ptr<const vector<size_t>>`; downstream operators cannot mutate a handed-off selection. Project is the materialization boundary for the supported slice: it walks the selected row ids in order, evaluates scalar expressions, and builds output columns through `ColumnarBatch::add_column`, preserving equal row counts and SELECT-list order.

## Design bias

The vectorized engine should never outrun the oracle. Add interpreted semantics first, then prove vectorized operators match it.
