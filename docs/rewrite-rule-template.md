# Rewrite Rule Template

For every optimizer rule, include:

- Pattern matched.
- Replacement expression.
- Semantic equivalence argument.
- Preconditions, especially around NULLs, duplicates, side effects, and ordering.
- For correlated rules, the explicit correlation-set/depth test, where every outer reference may occur, how substitution maps to join keys, and which residual/result-shaping locations block the rule.
- For predicate-tree rules, whether conjunct trees move as whole units, whether OR/AND splitting is forbidden, and what measure proves simplification or exploration terminates.
- Golden query that proves interpreted equality before/after rewrite.
