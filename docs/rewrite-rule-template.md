# Rewrite Rule Template

For every optimizer rule, include:

- Pattern matched.
- Replacement expression.
- Semantic equivalence argument.
- Preconditions, especially around NULLs, duplicates, side effects, and ordering.
- Golden query that proves interpreted equality before/after rewrite.
