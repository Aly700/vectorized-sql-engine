# Rewrite Rule Template

For every optimizer rule, include:

- Pattern matched.
- Replacement expression.
- Semantic equivalence argument.
- Preconditions, especially around NULLs, duplicates, side effects, and ordering.
- For predicate-tree rules, whether conjunct trees move as whole units, whether OR/AND splitting is forbidden, and what measure proves simplification or exploration terminates.
- Golden query that proves interpreted equality before/after rewrite.
