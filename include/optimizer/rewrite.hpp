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
// Preconditions: Only conjunct trees whose referenced binding identities are wholly available at
// the target child scope move below the join. Literal-only, unknown-scope, aggregate-output, and
// mixed-side non-leaf predicates stay residual; outer joins, volatile functions, and side effects
// do not exist in this slice; the original expression remains in the memo and ordering permission
// is preserved on the inserted alternative.
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
// aggregation, so predicates that reference them do not move.
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
// Preconditions: The expression must explicitly permit arbitrary row order; the plan context must
// have a final Project or another identity-addressed consumer that does not observe raw join column
// order; outer joins, side effects, and volatile expressions do not exist in the slice.
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
// Preconditions: The expression must explicitly permit arbitrary row order; every relocated
// conjunct must reference only tables available at its new join node; the newly created inner join
// must receive at least one predicate that connects its left and right children, so the rule does
// not introduce a cross-product-shaped intermediate; outer joins, side effects, and volatile
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
