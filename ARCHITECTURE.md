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
- `optimizer::rewrite_to_fixpoint` applies a deterministic ordered list of pure `LogicalPlan -> optional<LogicalPlan>` rewrite rules before physical lowering. The same rule proofs are also hosted by the memo exploration driver.
- `optimizer::Memo` stores logical equivalence groups. Group expressions reference child group ids, not nested plan objects. Ingest copies a bound logical tree bottom-up, assigns deterministic 1-based group ids, and deduplicates structurally identical expressions through structural hash/equality lookup.
- `plan::PhysicalPlan` is a structure-preserving lowering of the logical tree. It does not optimize, cost, or reorder.
- `execution::execute_interpreted` is the correctness oracle, including inner join semantics.
- `execution::execute_vectorized` lowers to the physical tree and runs scan/filter/join/project with immutable selection vectors until final materialization. Inner join materializes a deterministic joined batch boundary before downstream filters or projections run.
- `storage::ColumnarBatch` enforces equal-length column vectors.
- `execution::Catalog` implements `catalog::Catalog` for binding and separately owns table batches for execution.

## Memo core

The Phase 5a memo is a correctness-only Cascades core. A `MemoGroup` is a set of semantically equivalent `MemoExpression`s. Expressions are normalized into operator kind plus operator fields plus child group ids: scans carry a table name, filters and joins carry bound predicates, projects carry bound projections, and `GroupRef` explicitly records the checked case where a rule proves a group is equivalent to an existing child group.

Memo ids are deterministic because ingest is bottom-up and group ids are assigned from insertion order. The structural dedup table is a hash lookup only; observable behavior never depends on hash table iteration. Dumps print groups by id and expressions by insertion index.

Extraction has no costing in Phase 5a. It chooses the first-inserted expression in each group and recursively reconstructs a `LogicalPlan`. That canonical policy intentionally preserves the original ingested shape until Phase 5b adds costing and physical properties. Rule exploration still records additional equivalent expressions in the memo, and the differential corpus executes the extracted plan through both engines to prove extraction preserves semantics.

Memo boundaries fail loud. Insert, equivalent insertion, extraction, and dump validate that child group ids exist, expression arity matches operator kind, the structural index points back to the owning group, and group references do not introduce cycles.

## Rule-based rewrites

Rules live as proof-bearing classes with two hosts. The standalone host remains a pure transform from a `plan::LogicalPlan` to either an equivalent replacement plan or `std::nullopt`; tests that exercise that path continue to use it. The memo host applies the same proofs to `MemoExpression`s and inserts equivalent expressions into the same group. When `DropAlwaysTrueFilterRule` proves that a filter is equivalent to its child, it inserts an explicit `GroupRef` rather than silently merging groups.

The driver traverses child-first, tries rules in vector order, applies at most one rewrite per pass, records the fired rule name, and repeats to a fixpoint with a hard max-pass bound. For the shipped default rules, termination is monotonic: constant folding removes non-canonical literal comparisons by replacing them with canonical booleans; adjacent-filter merge removes a filter node while preserving predicate order; the always-false rule collapses a predicate list to one canonical false predicate; and the always-true rule removes predicates or an entire filter. No default rule reintroduces a non-canonical literal comparison, adjacent filter pair, canonical true predicate, or larger predicate list after simplification, so the bounded fixpoint is a guard against future cyclic rules rather than part of the proof.

Rewrite equivalence is tested in targeted rule tests by comparing unrewritten plans, standalone rewritten plans, and vectorized execution of the rewritten plans. The generated differential corpus now ingests each bound query into the memo, explores rules to fixpoint, extracts the deterministic canonical plan, and runs that plan through both engines against the unrewritten interpreted oracle. This protects the invariants that memo rules only add equivalent expressions and that vectorized execution matches the oracle for the same extracted logical plan.

Memo exploration walks groups by id, expressions by insertion index, and rules in `default_memo_rules()` order. It repeats until an iteration inserts no new expression. For the shipped default rules, termination follows from structural dedup plus monotonic simplification: constant folding only introduces canonical literal booleans, adjacent-filter merge removes a filter boundary, always-false canonicalization shortens a predicate list to canonical false, and always-true elimination removes true predicates or inserts a child `GroupRef`. The max-iteration bound is a guard for future rules.

The Phase 5a memo shape is compatible with Phase 5b join reordering because joins already reference child groups rather than child plans. Join commutativity and associativity can later add alternate join expressions into equivalent groups only when a proof accounts for bag semantics and output identity. Order-relaxed comparison is not enabled in 5a: the logical plan will need an explicit "arbitrary order permitted" property before a reordering rule may relax output order, and the oracle path will need bag-equality checking only for plans carrying that permission.

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

The interpreted oracle implements nested-loop inner join with bag semantics. The row order is part of the SQL engine contract: for each left row in input order, visit each right row in input order, emitting every pair whose `ON` conjuncts are true. Duplicate matches multiply, and an empty side yields zero output rows with the deterministic joined column order.

The vectorized join implements the same contract. It extracts usable equi-key conjuncts where `BoundColumnRef = BoundColumnRef` has exactly one side in each input. When such keys exist, it builds a hash table on the right input and stores selected right row ids in per-key vectors in right-input order. The unordered hash table is lookup-only: output is produced only by probing selected left rows in left-input order, then walking the matched right-row vector in insertion order. Hash bucket or table iteration must never influence result order. Non-equi, literal, same-side, or otherwise complex conjuncts remain residual predicates evaluated after a key match. If no usable equi conjunct exists, vectorized execution falls back to a nested-loop join with the oracle's left-row-major visitation order.

## Vectorized execution

Scan creates an identity selection vector in table row order. Filter evaluates predicates over the current selection vector and returns a newly allocated `shared_ptr<const vector<size_t>>`; downstream operators cannot mutate a handed-off selection. Join validates both child views, materializes a joined `ColumnarBatch`, then returns a fresh identity selection over that batch so downstream filters and projections cannot mutate child state. Project walks the selected row ids in order, evaluates scalar expressions, and builds output columns through `ColumnarBatch::add_column`, preserving equal row counts and SELECT-list order.

## Design bias

The vectorized engine should never outrun the oracle. Add interpreted semantics first, then prove vectorized operators match it.
