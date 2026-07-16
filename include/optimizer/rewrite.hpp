#pragma once

#include "optimizer/memo.hpp"
#include "plan/logical_plan.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace optimizer {

struct RewriteTrace {
    std::vector<std::string> fired_rules;
};

class Rule {
public:
    virtual ~Rule() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual std::optional<plan::LogicalPlan> apply(const plan::LogicalPlan& logical) const = 0;
};

class MemoRule {
public:
    virtual ~MemoRule() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const = 0;
};

struct RewriteOptions {
    std::size_t max_passes{32};
};

struct RewriteResult {
    plan::LogicalPlan plan;
    RewriteTrace trace;
    std::size_t passes{0};
    bool reached_fixpoint{false};
};

struct MemoExploreOptions {
    std::size_t max_iterations{32};
};

struct MemoExploreResult {
    std::vector<std::string> fired_rules;
    std::size_t iterations{0};
    bool reached_fixpoint{false};
};

// Pattern matched: Filter whose conjunct list contains predicate trees with non-canonical
// literal-vs-literal comparison leaves, or boolean subtrees that simplify against canonical TRUE
// (`lit(1) = lit(1)`) or canonical FALSE (`lit(1) = lit(0)`).
// Replacement expression: The same Filter with each matched comparison leaf replaced by canonical
// TRUE/FALSE, then with boolean tree algebra applied deterministically: TRUE OR x -> TRUE, FALSE OR
// x -> x, TRUE AND x -> x, FALSE AND x -> FALSE.
// Semantic equivalence argument: This rule folds only type-checked
// int64-literal-vs-int64-literal comparisons; string literals and `NULL` literals are not returned
// by the helper used for folding and therefore are never folded into TRUE/FALSE. A comparison
// between two int64 literals has a fixed non-UNKNOWN result for every input row. The boolean
// identities are valid for every SQL 3VL value x: TRUE OR x is TRUE, FALSE OR x is x, TRUE AND x
// is x, and FALSE AND x is FALSE. Replacing a subtree with the identical 3VL value preserves the
// rows whose final predicate is TRUE, and UNKNOWN rejection is unchanged. Each algebra
// simplification strictly reduces tree size; folding an int64 literal comparison replaces one
// non-canonical leaf with one canonical non-NULL leaf and no default rule reintroduces
// non-canonical literal comparisons.
// Preconditions: Folded canonical TRUE/FALSE leaves are non-NULL int64 comparisons; predicates are
// pure and side-effect-free; duplicate rows are preserved because this rule only changes per-row
// predicate truth values; input and output ordering are preserved because the node shape and child
// remain unchanged.
// Golden query: `SELECT a FROM t WHERE 2 > 1 OR a = 2` proves interpreted equality before and
// after replacing `2 > 1` with canonical TRUE and simplifying the OR tree.
class ConstantFoldComparisonRule final : public Rule, public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "ConstantFoldComparisonRule"; }
    [[nodiscard]] std::optional<plan::LogicalPlan> apply(const plan::LogicalPlan& logical) const override;
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: Filter whose conjunct list contains canonical TRUE (`lit(1) = lit(1)`).
// Replacement expression: Remove TRUE conjuncts; if all conjuncts are TRUE, replace the Filter
// with its child.
// Semantic equivalence argument: Canonical TRUE is a non-NULL predicate leaf. In SQL 3VL,
// `TRUE AND p` has exactly the same truth value as `p` for p in {TRUE, FALSE, UNKNOWN}, and a filter
// containing only TRUE accepts every input row. Bag multiplicities and duplicate rows are unchanged
// because no rows are introduced or collapsed.
// Preconditions: Canonical TRUE has already been introduced by a sound rule or by an equivalent
// bound predicate; predicates are pure with no side effects or volatile expressions; predicate
// evaluation order is not observable in the current slice, and surviving conjunct order is
// preserved.
// Golden query: `SELECT a FROM t WHERE 2 > 1 AND a = 2` proves interpreted equality before and
// after dropping the folded TRUE conjunct.
class DropAlwaysTrueFilterRule final : public Rule, public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "DropAlwaysTrueFilterRule"; }
    [[nodiscard]] std::optional<plan::LogicalPlan> apply(const plan::LogicalPlan& logical) const override;
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: Filter whose conjunct list contains canonical FALSE (`lit(1) = lit(0)`).
// Replacement expression: Replace the conjunct list with only canonical FALSE, yielding the
// canonical empty-relation form for this plan slice.
// Semantic equivalence argument: Canonical FALSE is a non-NULL predicate leaf. In SQL 3VL,
// `FALSE AND p` is FALSE for p in {TRUE, FALSE, UNKNOWN}, so the output bag is empty regardless of
// input duplicates. The current plan algebra has no explicit Empty node, so the canonical FALSE
// filter is the pure logical representation of an empty relation.
// Preconditions: Canonical FALSE has already been introduced by a sound rule or by an equivalent
// bound predicate; predicates are pure with no side effects or volatile expressions; no ordering
// guarantee is lost because the result has zero rows.
// Golden query: `SELECT a FROM t WHERE a = 2 AND 2 < 1` proves interpreted equality before and
// after replacing the filter with canonical FALSE.
class AlwaysFalseFilterRule final : public Rule, public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "AlwaysFalseFilterRule"; }
    [[nodiscard]] std::optional<plan::LogicalPlan> apply(const plan::LogicalPlan& logical) const override;
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: Filter whose child is also a Filter.
// Replacement expression: One Filter over the grandchild with inner predicates followed by outer
// predicates.
// Semantic equivalence argument: Applying filter `p` and then filter `q` keeps exactly rows where
// `p` is TRUE and `q` is TRUE. In SQL 3VL, `p AND q` is TRUE exactly in that same case; FALSE and
// UNKNOWN results are rejected by both shapes. Preserving inner-before-outer predicate order keeps
// the current deterministic evaluation sequence. Bag multiplicities are unchanged because rows are
// only retained or rejected.
// Preconditions: Predicates are pure with no side effects or volatility; duplicates are not
// collapsed; ordering is preserved because the same child row order flows through a single
// equivalent filter.
// Golden query: `SELECT a FROM t WHERE a >= 2 AND b < 40` proves interpreted equality for the same
// conjunction; the targeted ctest builds the adjacent-filter shape manually because the current
// binder already emits a single Filter for SQL conjuncts.
class MergeAdjacentFiltersRule final : public Rule, public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "MergeAdjacentFiltersRule"; }
    [[nodiscard]] std::optional<plan::LogicalPlan> apply(const plan::LogicalPlan& logical) const override;
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: a Filter with a top-level EXISTS subquery conjunct.
// Replacement expression: remove that conjunct and place the Filter input on
// the preserved side of a keyless SemiJoin whose right side is the subquery;
// keep any residual top-level conjuncts above the join.
// Semantic equivalence: EXISTS is TRUE exactly when its independently bound
// right plan has a row. A keyless SemiJoin treats its empty predicate list as
// TRUE for every pair and emits a left row once iff any right row exists, so it
// preserves bag multiplicity and left order exactly.
// Preconditions: only an entire top-level Filter conjunct is matched; EXISTS
// inside AND/OR predicate trees is not inspected or split. The subquery is
// uncorrelated, immutable, pure, and non-null. The rule removes one subquery
// leaf from the owning Filter and never creates one, while structural memo
// deduplication closes repeated exploration.
// Golden query: `SELECT a FROM t WHERE EXISTS (SELECT a FROM t1)`.
class ExistsToSemiJoinRule final : public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "ExistsToSemiJoinRule"; }
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: a Filter with a top-level NOT EXISTS subquery conjunct.
// Replacement expression: remove that conjunct and place the Filter input on
// the preserved side of a keyless AntiJoin to the subquery, retaining residual
// top-level conjuncts above it.
// Semantic equivalence: NOT EXISTS is TRUE exactly when the right plan is
// empty. Keyless AntiJoin emits each left row once iff no TRUE pair exists;
// right values and NULLs are irrelevant, so no NULL trap exists.
// Preconditions: the predicate itself must be one top-level Filter conjunct;
// trees below AND/OR are opaque. The independently bound subquery is immutable,
// pure, and non-null. Each application removes one eligible leaf and memo
// structural deduplication guarantees finite exploration.
// Golden query: `SELECT a FROM t WHERE NOT EXISTS (SELECT a FROM empty)`.
class NotExistsToAntiJoinRule final : public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "NotExistsToAntiJoinRule"; }
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: a Filter with a top-level `x IN (SELECT c ...)` conjunct.
// Replacement expression: remove that conjunct and add SemiJoin(input,
// subquery, x = c), retaining residual top-level conjuncts above the join.
// Semantic equivalence: under TRUE-only filtering, IN survives iff at least
// one non-NULL c makes x=c TRUE. Equi-SemiJoin has exactly that match set and
// skips NULL equality keys. When x is NULL, or no value matches but the set has
// a NULL, IN is UNKNOWN while SemiJoin reports no match; both reject the left
// row. Right duplicates cannot multiply a Semi result.
// Preconditions: the IN leaf must be a whole top-level Filter conjunct; IN
// inside OR/AND is never split. NOT IN is excluded because a NULL-bearing set
// makes NOT IN never TRUE while plain AntiJoin would emit unmatched rows.
// Scalar subquery operands are excluded and remain eagerly materialized. The
// subquery has exactly one typed output by binder invariant.
// Golden query: `SELECT a FROM t WHERE a IN (SELECT a FROM t1)`.
class InToSemiJoinRule final : public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "InToSemiJoinRule"; }
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: one whole top-level `x NOT IN (SELECT c FROM Q)` Filter
// conjunct whose left operand is not a scalar subquery and whose bound right
// plan is uncorrelated with exactly one typed final output.
// Replacement expression: retain the materialized Filter expression and add
// NullAwareAnti(input, Q) with an empty candidate-predicate list and the
// structural membership equality `x = c`; residual top-level conjuncts remain
// above the new join. If Q's projected identity collides with a left identity,
// a deterministic `__not_in_value_N` Project alias makes the right side
// explicit before constructing that equality.
// Semantic equivalence: for each left row, the join candidate bag is exactly
// Q. An empty Q emits, matching TRUE even for NULL x. For nonempty Q, NULL x
// makes every equality UNKNOWN and suppresses. A TRUE equality suppresses as
// NOT IN FALSE; absent a TRUE, any NULL c supplies UNKNOWN and suppresses; only
// non-NULL x with every non-NULL c unequal yields all FALSE and emits. These
// cases exhaust SQL 3VL. Right duplicates repeat an existing truth value and
// cannot multiply the left-only result. The join preserves left row order and
// executes the right plan once eagerly, so empty-left behavior and positioned
// checked-expression errors coincide with materialization.
// Preconditions/guards: only an entire Filter conjunct is inspected; leaves
// below AND/OR, scalar-subquery operands, correlation, missing plans, or a
// non-single-output bound shape do not rewrite. A sibling subquery predicate
// or an embedded subquery in the left relational input also blocks the rule:
// materialization observes Q's eager execution relative to those other eager
// errors, and a native join cannot in general reproduce that owner ordering.
// Binding has already pinned any checked SUM input, and memo ingest copies
// every node's order permission.
// Unsupported correlation stays materialized and physical lowering fails at
// its deterministic source position. Each application removes one NOT IN leaf
// from the added expression, never creates one, and structural deduplication
// guarantees termination.
// Golden query: `SELECT a FROM t WHERE a NOT IN (SELECT a FROM t1)` explores
// both materialized and NullAwareAnti alternatives; the trap matrix separately
// pins empty and NULL-bearing right sets.
class NotInToNullAwareAntiJoinRule final : public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override {
        return "NotInToNullAwareAntiJoinRule";
    }
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: a top-level Filter EXISTS whose correlated subplan is
// exactly Project(Filter(Q)) and every outer reference occurs in a whole,
// top-level `outer_col = inner_col` WHERE conjunct.
// Replacement expression: remove those conjuncts from Q, project deterministic
// hidden inner keys, and add SemiJoin(outer, decorrelated-Q, outer=hidden-inner).
// Semantic equivalence: for each outer row, substituting its correlation values
// accepts exactly inner rows whose equality keys are TRUE. Equi-SemiJoin tests
// the same TRUE matches and emits the outer row once iff one exists. A NULL on
// either key makes equality UNKNOWN, hence no Semi match in both forms.
// Preconditions: correlations must be immediate-parent/local column equality;
// non-equality, AND/OR-contained, projection, aggregate, HAVING, ON, nested,
// DISTINCT/ORDER/LIMIT, and any residual correlation block the rule. Existing
// projections remain evaluated and bag/order of the preserved side is unchanged.
// Golden query: `SELECT a FROM t WHERE EXISTS (SELECT b FROM t1 WHERE t1.a=t.a)`.
class CorrelatedExistsToSemiJoinRule final : public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "CorrelatedExistsToSemiJoinRule"; }
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern/replacement/preconditions are identical to
// CorrelatedExistsToSemiJoinRule except NOT EXISTS becomes an equi AntiJoin.
// Semantic equivalence: Anti emits an outer row exactly when no inner equality
// is TRUE. FALSE and UNKNOWN are both non-matches, so a NULL correlation value
// is retained, exactly as per-row NOT EXISTS over an UNKNOWN-rejected WHERE.
// Golden query: `SELECT a FROM t WHERE NOT EXISTS (SELECT b FROM t1 WHERE t1.a=t.a)`.
class CorrelatedNotExistsToAntiJoinRule final : public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "CorrelatedNotExistsToAntiJoinRule"; }
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: the same equality-only correlated subplan owned by a whole
// top-level `x IN (SELECT c ...)` Filter conjunct.
// Replacement: an equi SemiJoin with the extracted correlation keys plus x=c.
// Semantic equivalence: the filter survives only when some correlation-matched
// row makes x=c TRUE; Semi has the same TRUE match set. NULL x, NULL c, and NULL
// correlation keys cannot match and are rejected in both forms. NOT IN is
// excluded because a NULL-bearing right set requires null-aware anti semantics.
// All shape/precondition guards from the correlated EXISTS rule apply.
// Golden query: `SELECT b FROM t WHERE b IN (SELECT b FROM t1 WHERE t1.a=t.a)`.
class CorrelatedInToSemiJoinRule final : public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "CorrelatedInToSemiJoinRule"; }
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: a whole top-level `x NOT IN (SELECT c ...)` conjunct whose
// correlated subplan is exactly the Phase 21c Project(Filter(Scan|Join)) shape,
// with every outer use covered by an immediate-parent/local column equality.
// Replacement: remove the correlation WHERE equalities from a copied right
// plan, append deterministic `__corr_key_N` projections, keep the resulting
// outer-to-hidden equalities as candidate predicates, and store `x = c`
// separately as the NullAwareAnti membership equality. A colliding selected
// right identity receives the same deterministic hidden alias. The original
// materialized Filter remains in its memo group.
// Semantic equivalence: for a fixed outer row l, TRUE-only evaluation of the
// extracted candidate equalities selects exactly the same inner bag C(l) as
// substituting l into the original WHERE. A NULL on either correlation key
// makes that equality UNKNOWN and therefore can make C(l) empty; it never acts
// as a membership NULL. Applying the exhaustive uncorrelated NOT IN argument
// above to each C(l) proves identical TRUE/FALSE/UNKNOWN survival for every
// row, including empty per-key bags, NULL membership values, and NULL outer
// correlation keys. Hidden columns do not escape the left-only join, right
// duplicates do not multiply rows, and left-row-major order is unchanged.
// Preconditions/guards: scalar-subquery operands; non-equality, nested,
// transitive, projection, aggregate, HAVING, JOIN ON, AND/OR-contained, and
// result-shaped correlation are rejected. Nested subqueries in the right plan
// and sibling subquery predicates are also rejected because decorrelation
// would change their eager/per-row error order. The accepted
// Scan|Join/Filter/Project right slice is therefore pure and has no
// runtime-error-producing aggregate; all copied order permissions remain
// structural. Residual shapes stay materialized and fail physical lowering
// with the positioned guard.
// The replacement removes one eligible leaf and memo deduplication guarantees
// termination.
// Golden query: `SELECT b FROM t WHERE b NOT IN (SELECT t1.b FROM t1 WHERE
// t1.a=t.a)` executes the original and every native alternative through both
// engines.
class CorrelatedNotInToNullAwareAntiJoinRule final : public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override {
        return "CorrelatedNotInToNullAwareAntiJoinRule";
    }
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: Filter(P, LeftJoin(L, R, J)) where at least one whole top-level conjunct in P
// is provably null-rejecting for R's binding identities.
// Replacement expression: Add Filter(P, InnerJoin(L, R, J)) as an equivalent expression in the
// Filter group. The new INNER join is created in its own group; the bare LEFT and INNER joins are
// never declared equivalent. The original filtered LEFT expression remains in the memo.
// Semantic equivalence argument: A LEFT join emits every TRUE-matching pair and emits one
// NULL-extended right row only for a left row with no TRUE ON match. INNER emits the same matching
// pairs and omits only those NULL-extended rows. On a NULL-extended row, a comparison that reads any
// R column has a NULL operand and is UNKNOWN, so Filter rejects it. Recursively, an AND tree is
// null-rejecting if either child is null-rejecting because an AND with a child that cannot be TRUE
// cannot be TRUE; an OR tree is null-rejecting only if both children are null-rejecting because any
// non-rejecting disjunct could make the OR TRUE. IS NULL and IS NOT NULL leaves are deliberately
// outside this comparison-rooted conservative proof; in particular R.x IS NULL is TRUE on the
// NULL-extended row and cannot justify the rewrite. Therefore P removes exactly the rows on which
// LEFT and INNER differ, while preserving every matched pair and its bag multiplicity.
// Preconditions: The Filter child expression must be a LEFT join; right-side membership is proved
// from the right child group's bound binding identities; predicate trees move only as whole
// conjuncts and no OR/AND splitting occurs; comparisons and predicates are pure and type-checked;
// Filter and join order permission is preserved. Exploration terminates because the rule adds only
// the structurally deduplicated filtered-INNER alternative and never converts INNER back to LEFT.
// Golden query: `SELECT t1.a, t1.b, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a WHERE t2.c = 201`
// runs the original, every memo alternative, and extract_best through both engines. The companion
// EXPLAIN golden over big/mid/tiny pins LEFT-to-INNER plus the commute/associate alternatives it
// unlocks; targeted negatives pin IS NULL and left-only filters.
class LeftJoinToInnerRule final : public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "LeftJoinToInnerRule"; }
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: Filter(P, Join(L, R, J)) in the memo.
// Replacement expression: An equivalent join alternative where each filter conjunct that references
// only L's binding identities becomes/merges with a Filter over L, each conjunct that references only
// R's binding identities becomes/merges with a Filter over R, each comparison-leaf conjunct that
// references both sides is appended to the Join predicate list, and residual conjuncts remain in a
// smaller Filter above the new Join. Predicate trees move only as whole conjuncts; mixed-side OR/AND
// trees are not split and stay residual. If no residual conjunct remains, the Filter disappears.
// Semantic equivalence argument: For inner join under bag semantics and TRUE-only filter/join
// semantics, applying a pure one-side predicate before the join preserves exactly the matching pair
// multiplicities that would survive filtering after the join: TRUE rows keep all candidate pairs,
// while FALSE and UNKNOWN rows produce no accepted pair in either shape. Bound predicate leaves are
// already type-checked, so moving a whole conjunct does not change comparison type, lexicographic
// string behavior, or NULL handling. A comparison leaf that reads both inputs is semantically a join
// predicate because it is evaluated over the same row pair at a point where both rows are available;
// NULL operands yield UNKNOWN and are rejected in either placement. Whole-tree reference analysis
// prevents unsound OR/AND splitting.
// Preconditions: The child join must be an INNER join, except that a whole
// left-only conjunct may move into the preserved child of a SEMI, ANTI, or
// NULL-aware ANTI join. This exception is sound because every preserved-left
// join output is a subset of left rows:
// filtering a left row before testing match existence keeps exactly the rows
// that would pass the same predicate after the existence test. Right-only,
// mixed-side, literal-only, and empty-reference conjuncts remain above a
// preserved-left join; no predicate is moved into its candidate conditions,
// NULL-aware membership equality, match condition, or right side.
// LEFT joins are skipped because pushing a
// predicate into the null-supplying side, or turning a post-join predicate into an ON predicate,
// can preserve NULL-extended rows that the original WHERE would reject or reject rows the original
// ON would preserve. LeftJoinToInnerRule may first establish an equivalent filtered INNER shape;
// this rule still never weakens its own join-kind guard. Only conjunct trees whose referenced
// binding identities are wholly available at the target
// child scope move below the join. Literal-only, unknown-scope, aggregate-output, and mixed-side
// non-leaf predicates stay residual; predicates are pure, and the original expression remains in
// the memo with ordering permission preserved on the inserted alternative.
// Golden query: `SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a WHERE t1.b = 20 AND t2.c > 200
// AND t1.b < t2.c` proves all pushed alternatives remain equal to the unrewritten oracle.
class FilterIntoJoinRule final : public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "FilterIntoJoinRule"; }
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: Filter(P, Aggregate(group_keys, aggregates, input)) in the memo.
// Replacement expression: Push each whole conjunct tree whose column references are exactly
// grouping-key identities into a new/merged Filter over the Aggregate input, then rebuild the
// Aggregate above that filtered input. Aggregate-output and other residual conjunct trees stay in a
// smaller Filter above the Aggregate; if no residual remains, the Filter disappears.
// Semantic equivalence argument: Group membership is decided per input row solely by grouping-key
// values, and those key values are constant for every row in a group, including NULL key values
// once GROUP BY NULL semantics are fully specified. Filtering groups by a predicate over only those
// keys keeps exactly groups whose key predicate is TRUE; filtering input rows by the same predicate
// before aggregation keeps exactly the rows for those TRUE groups. FALSE and UNKNOWN key predicates
// reject the group or input rows in both shapes. Bound grouping-key predicates carry their checked
// key type, so string and int64 keys move under the same whole-conjunct argument without coercion.
// Aggregate outputs such as COUNT/SUM are computed after grouping and are not available before
// aggregation, so predicates that reference them do not move. This rule is unaffected by LEFT joins
// because it matches only Filter over Aggregate and does not reinterpret join null-extension below
// the Aggregate input.
// Preconditions: Every moved conjunct tree must reference at least one column and every referenced
// column in every leaf must match one of `group_keys` by bound identity. The current binder
// represents HAVING grouping-key predicates with the original input `BoundColumnRef{binding,
// column}` and aggregate outputs with an empty binding, so any aggregate-output leaf fails this
// exact match and pins the whole tree. Predicates are pure and side-effect-free; duplicates are
// preserved because aggregation still sees exactly the filtered input row bag. The 16a fuzzer avoids
// NULL-affected GROUP BY cases until Phase 16b, but the proof obligation for moved grouping-key
// predicates is 3VL TRUE-equivalence, not two-valued logic.
// Golden query: `SELECT t1.a, COUNT(*) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.a HAVING
// t1.a = 2 AND COUNT(*) > 1` proves the grouping-key conjunct moves while COUNT(*) stays above.
class FilterThroughAggregateRule final : public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "FilterThroughAggregateRule"; }
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: Join(A, B, predicates) in an arbitrary-order, identity-addressed memo context.
// Replacement expression: Join(B, A, predicates).
// Semantic equivalence argument: Inner join is symmetric under SQL bag semantics and TRUE-only
// join predicate semantics: each row pair whose predicate tree is TRUE appears with the same
// multiplicity after swapping the inputs, and pairs whose predicates are FALSE or UNKNOWN are
// rejected in both orders. Predicates are bound to stable table/column identities rather than
// positions, so their 3VL truth values are unchanged. NULL equi keys do not match in either order
// because equality with NULL is UNKNOWN. The join node's internal output identity order flips, but
// admitted bound SQL plans consume join columns by identity and the final Project fixes
// user-visible output names/order.
// Preconditions: The expression must be an INNER join and must explicitly permit arbitrary row
// order; the plan context must have a final Project or another identity-addressed consumer that
// does not observe raw join column order. LEFT joins do not commute because swapping inputs changes
// which side is preserved and which side is NULL-extended. Side effects and volatile expressions do
// not exist in the slice.
// Golden query: `SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a` proves sorted-bag equality for
// commuted memo alternatives.
class JoinCommuteRule final : public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "JoinCommuteRule"; }
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: Join(Join(A, B, p_ab), C, p_abc) or Join(A, Join(B, C, p_bc), p_abc)
// in an arbitrary-order, identity-addressed memo context.
// Replacement expression: The opposite association, with whole conjunct trees placed only at join
// nodes where all referenced table identities are available.
// Semantic equivalence argument: Inner join over pure SQL 3VL predicates is associative under bag
// semantics when the same whole conjuncts are evaluated after both sides they reference are
// present. For each full row tuple, every conjunct receives the same inputs and therefore the same
// TRUE/FALSE/UNKNOWN result in both associations; the tuple is emitted exactly when all required
// join predicates are TRUE. Multiplicities are preserved because no duplicate-eliminating operator
// is introduced.
// Preconditions: Every join rotated by the rule must be INNER and the outer expression must
// explicitly permit arbitrary row order; every relocated conjunct must reference only tables
// available at its new join node; the newly created inner join must receive at least one predicate
// that connects its left and right children, so the rule does not introduce a cross-product-shaped
// intermediate. INNER/LEFT and LEFT/LEFT associativity are invalid in general because
// NULL-extension timing and preserved-side identity can change. Side effects and volatile
// expressions do not exist in the slice.
// Golden query: `SELECT t1.b, t2.c, t3.d FROM t1 JOIN t2 ON t1.a = t2.a JOIN t3 ON t2.c = t3.c`
// proves sorted-bag equality for associated memo alternatives.
class JoinAssociateRule final : public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "JoinAssociateRule"; }
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

[[nodiscard]] RewriteResult rewrite_to_fixpoint(
    const plan::LogicalPlan& logical,
    const std::vector<std::reference_wrapper<const Rule>>& rules,
    RewriteOptions options = {});

[[nodiscard]] std::vector<std::reference_wrapper<const Rule>> default_rules();
[[nodiscard]] MemoExploreResult explore_memo_to_fixpoint(
    Memo& memo,
    const std::vector<std::reference_wrapper<const MemoRule>>& rules,
    MemoExploreOptions options = {});
[[nodiscard]] std::vector<std::reference_wrapper<const MemoRule>> default_memo_rules();

} // namespace optimizer
