#pragma once

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

struct RewriteOptions {
    std::size_t max_passes{32};
};

struct RewriteResult {
    plan::LogicalPlan plan;
    RewriteTrace trace;
    std::size_t passes{0};
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
class ConstantFoldComparisonRule final : public Rule {
public:
    [[nodiscard]] std::string_view name() const override { return "ConstantFoldComparisonRule"; }
    [[nodiscard]] std::optional<plan::LogicalPlan> apply(const plan::LogicalPlan& logical) const override;
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
class DropAlwaysTrueFilterRule final : public Rule {
public:
    [[nodiscard]] std::string_view name() const override { return "DropAlwaysTrueFilterRule"; }
    [[nodiscard]] std::optional<plan::LogicalPlan> apply(const plan::LogicalPlan& logical) const override;
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
class AlwaysFalseFilterRule final : public Rule {
public:
    [[nodiscard]] std::string_view name() const override { return "AlwaysFalseFilterRule"; }
    [[nodiscard]] std::optional<plan::LogicalPlan> apply(const plan::LogicalPlan& logical) const override;
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
class MergeAdjacentFiltersRule final : public Rule {
public:
    [[nodiscard]] std::string_view name() const override { return "MergeAdjacentFiltersRule"; }
    [[nodiscard]] std::optional<plan::LogicalPlan> apply(const plan::LogicalPlan& logical) const override;
};

[[nodiscard]] RewriteResult rewrite_to_fixpoint(
    const plan::LogicalPlan& logical,
    const std::vector<std::reference_wrapper<const Rule>>& rules,
    RewriteOptions options = {});

[[nodiscard]] std::vector<std::reference_wrapper<const Rule>> default_rules();

} // namespace optimizer
