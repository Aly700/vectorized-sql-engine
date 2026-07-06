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

// Pattern matched: Filter whose conjunct list contains non-canonical literal-vs-literal comparisons.
// Replacement expression: The same Filter with each matched comparison replaced by canonical TRUE
// (`lit(1) = lit(1)`) or canonical FALSE (`lit(1) = lit(0)`).
// Semantic equivalence argument: The current scalar expression slice has only pure non-NULL int64
// literals and column reads. A comparison between two literals has a fixed two-valued result for
// every input row, so replacing it with an equivalent canonical boolean predicate preserves the
// accepted row bag exactly.
// Preconditions: No NULLs, no volatile functions, and no side effects exist in the current slice;
// duplicate rows are preserved because this rule only changes per-row predicate truth values;
// input and output ordering are preserved because the node shape and child remain unchanged.
// Golden query: `SELECT a FROM t WHERE 2 > 1 AND a = 2` proves interpreted equality before and
// after replacing `2 > 1` with canonical TRUE.
class ConstantFoldComparisonRule final : public Rule, public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "ConstantFoldComparisonRule"; }
    [[nodiscard]] std::optional<plan::LogicalPlan> apply(const plan::LogicalPlan& logical) const override;
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: Filter whose conjunct list contains canonical TRUE (`lit(1) = lit(1)`).
// Replacement expression: Remove TRUE conjuncts; if all conjuncts are TRUE, replace the Filter
// with its child.
// Semantic equivalence argument: Under the current two-valued, side-effect-free predicate
// semantics, `TRUE AND p` accepts exactly the same rows as `p`, and a filter containing only TRUE
// accepts every input row. Bag multiplicities and duplicate rows are unchanged because no rows are
// introduced or collapsed.
// Preconditions: Canonical TRUE has already been introduced by a sound rule or by an equivalent
// bound predicate; no NULLs, side effects, or volatile expressions exist; predicate evaluation order
// is not observable in the current slice, and surviving conjunct order is preserved.
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
// Semantic equivalence argument: `FALSE AND p` rejects every input row, so the output bag is empty
// regardless of input duplicates. The current plan algebra has no explicit Empty node, so the
// canonical FALSE filter is the pure logical representation of an empty relation.
// Preconditions: Canonical FALSE has already been introduced by a sound rule or by an equivalent
// bound predicate; no NULLs, side effects, or volatile expressions exist; no ordering guarantee is
// lost because the result has zero rows.
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
// Semantic equivalence argument: Applying filter `p` and then filter `q` accepts exactly rows that
// satisfy `p AND q`; preserving inner-before-outer predicate order keeps the current deterministic
// evaluation sequence. Bag multiplicities are unchanged because rows are only retained or rejected.
// Preconditions: Predicates are pure, non-NULL two-valued comparisons with no side effects or
// volatility; duplicates are not collapsed; ordering is preserved because the same child row order
// flows through a single equivalent filter.
// Golden query: `SELECT a FROM t WHERE a >= 2 AND b < 40` proves interpreted equality for the same
// conjunction; the targeted ctest builds the adjacent-filter shape manually because the current
// binder already emits a single Filter for SQL conjuncts.
class MergeAdjacentFiltersRule final : public Rule, public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "MergeAdjacentFiltersRule"; }
    [[nodiscard]] std::optional<plan::LogicalPlan> apply(const plan::LogicalPlan& logical) const override;
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: Join(A, B, predicates) in an arbitrary-order, identity-addressed memo context.
// Replacement expression: Join(B, A, predicates).
// Semantic equivalence argument: Inner join is symmetric under SQL bag semantics: each matching
// pair from A and B appears with the same multiplicity after swapping the inputs. Predicates are
// bound to stable table/column identities rather than positions, so their truth values are
// unchanged. The join node's internal output identity order flips, but admitted bound SQL plans
// consume join columns by identity and the final Project fixes user-visible output names/order.
// Preconditions: The expression must explicitly permit arbitrary row order; the plan context must
// have a final Project or another identity-addressed consumer that does not observe raw join column
// order; no NULLs, outer joins, side effects, or volatile expressions exist in the slice.
// Golden query: `SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a` proves sorted-bag equality for
// commuted memo alternatives.
class JoinCommuteRule final : public MemoRule {
public:
    [[nodiscard]] std::string_view name() const override { return "JoinCommuteRule"; }
    [[nodiscard]] bool apply(Memo& memo, GroupId group, const MemoExpression& expression) const override;
};

// Pattern matched: Join(Join(A, B, p_ab), C, p_abc) or Join(A, Join(B, C, p_bc), p_abc)
// in an arbitrary-order, identity-addressed memo context.
// Replacement expression: The opposite association, with conjuncts placed only at join nodes where
// all referenced table identities are available.
// Semantic equivalence argument: Inner join over pure two-valued predicates is associative under
// bag semantics when the same conjuncts are evaluated after both sides they reference are present.
// Multiplicities are preserved because no duplicate-eliminating operator is introduced.
// Preconditions: The expression must explicitly permit arbitrary row order; every relocated
// conjunct must reference only tables available at its new join node; the newly created inner join
// must receive at least one predicate that connects its left and right children, so the rule does
// not introduce a cross-product-shaped intermediate; no NULLs, outer joins, side effects, or
// volatile expressions exist in the slice.
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
