# Architecture — Vectorized SQL Query Engine

## Pipeline

```text
SQL text -> parser -> binder/type checker -> logical plan -> optimizer memo -> physical plan -> vectorized execution
```

## Current scaffold

- `sql::parse_select` implements a small, explicit SQL slice for golden tests and rejects unsupported syntax with position-bearing parse errors.
- `sql::bind_select` resolves parsed column references against one catalog table scope and rejects binding errors before logical planning.
- `plan::LogicalPlan` is the post-binding algebra handoff boundary.
- `plan::PhysicalPlan` is a structure-preserving lowering of the logical tree. It does not optimize, cost, or reorder.
- `execution::execute_interpreted` is the correctness oracle.
- `execution::execute_vectorized` lowers to the physical tree and runs scan/filter/project with immutable selection vectors until final materialization.
- `storage::ColumnarBatch` enforces equal-length column vectors.
- `optimizer::Memo` is the shell for Cascades groups.

## Vectorized execution

Scan creates an identity selection vector in table row order. Filter evaluates predicates over the current selection vector and returns a newly allocated `shared_ptr<const vector<size_t>>`; downstream operators cannot mutate a handed-off selection. Project is the materialization boundary for the supported slice: it walks the selected row ids in order, evaluates scalar expressions, and builds output columns through `ColumnarBatch::add_column`, preserving equal row counts and SELECT-list order.

## Known debt

- `sql::bind_select` binds directly against `execution::Catalog`. Acceptable while the catalog is one flat table map; before the optimizer phase, binding must target a schema/catalog abstraction so the sql layer stops depending on the execution layer.

## Design bias

The vectorized engine should never outrun the oracle. Add interpreted semantics first, then prove vectorized operators match it.
