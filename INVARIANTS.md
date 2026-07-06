# Invariants — Vectorized SQL Query Engine

## SQL semantics

- Binding resolves every column reference against exactly one scope.
- Rewrites must preserve bag semantics, NULL behavior, and expression evaluation order where SQL requires it.
- Type coercions must be explicit after binding.

## Optimizer

- Every memo group represents semantically equivalent expressions.
- A rule may only insert an expression into an existing group if an equivalence proof is documented in the rule comment.
- Costing may choose among equivalents; it may not create semantics.

## Execution

- Vectorized operators must produce the same result as the interpreted oracle for the same logical plan.
- Column vectors in a batch have identical row counts.
- Selection vectors and masks are immutable once handed to a downstream operator.
- Operator output order is deterministic unless the logical plan explicitly permits arbitrary order.
