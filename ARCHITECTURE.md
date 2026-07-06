# Architecture — Vectorized SQL Query Engine

## Pipeline

```text
SQL text -> parser -> binder/type checker -> logical plan -> optimizer memo -> physical plan -> vectorized execution
```

## Current scaffold

- `sql::parse_select` implements a deliberately tiny SQL slice for golden tests.
- `plan::LogicalPlan` is the algebra handoff boundary.
- `execution::execute_interpreted` is the correctness oracle.
- `storage::ColumnarBatch` enforces equal-length column vectors.
- `optimizer::Memo` is the shell for Cascades groups.

## Design bias

The vectorized engine should never outrun the oracle. Add interpreted semantics first, then prove vectorized operators match it.
