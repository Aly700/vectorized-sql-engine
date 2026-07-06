# Architecture — Vectorized SQL Query Engine

## Pipeline

```text
SQL text -> parser -> binder/type checker -> logical plan -> optimizer memo -> physical plan -> vectorized execution
```

## Current scaffold

- `sql::parse_select` implements a small, explicit SQL slice for golden tests and rejects unsupported syntax with position-bearing parse errors.
- `sql::bind_select` resolves parsed column references against one catalog table scope and rejects binding errors before logical planning.
- `plan::LogicalPlan` is the post-binding algebra handoff boundary.
- `execution::execute_interpreted` is the correctness oracle.
- `storage::ColumnarBatch` enforces equal-length column vectors.
- `optimizer::Memo` is the shell for Cascades groups.

## Known debt

- `sql::bind_select` binds directly against `execution::Catalog`. Acceptable while the catalog is one flat table map; before the optimizer phase, binding must target a schema/catalog abstraction so the sql layer stops depending on the execution layer.

## Design bias

The vectorized engine should never outrun the oracle. Add interpreted semantics first, then prove vectorized operators match it.
