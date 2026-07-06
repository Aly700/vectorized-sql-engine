#include "optimizer/rewrite.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <variant>

namespace optimizer {
namespace {

const plan::LogicalPlan& require_input(const plan::LogicalPlan& logical) {
    if (!logical.input) {
        throw std::invalid_argument("logical plan node is missing its input");
    }
    return *logical.input;
}

const plan::LogicalPlan& require_left(const plan::LogicalPlan& logical) {
    if (!logical.left) {
        throw std::invalid_argument("logical join node is missing its left input");
    }
    return *logical.left;
}

const plan::LogicalPlan& require_right(const plan::LogicalPlan& logical) {
    if (!logical.right) {
        throw std::invalid_argument("logical join node is missing its right input");
    }
    return *logical.right;
}

std::optional<std::int64_t> literal_value(const plan::BoundScalarExpr& expression) {
    if (const auto* literal = std::get_if<sql::IntLiteral>(&expression)) {
        return literal->value;
    }
    return std::nullopt;
}

bool compare_values(std::int64_t left, sql::ComparisonOp op, std::int64_t right) {
    switch (op) {
    case sql::ComparisonOp::Equal:
        return left == right;
    case sql::ComparisonOp::NotEqual:
        return left != right;
    case sql::ComparisonOp::Less:
        return left < right;
    case sql::ComparisonOp::LessEqual:
        return left <= right;
    case sql::ComparisonOp::Greater:
        return left > right;
    case sql::ComparisonOp::GreaterEqual:
        return left >= right;
    }
    throw std::logic_error("unreachable comparison operator");
}

plan::BoundComparisonExpr canonical_true() {
    return plan::BoundComparisonExpr{
        sql::IntLiteral{1, 0},
        sql::ComparisonOp::Equal,
        sql::IntLiteral{1, 0},
        0,
    };
}

plan::BoundComparisonExpr canonical_false() {
    return plan::BoundComparisonExpr{
        sql::IntLiteral{1, 0},
        sql::ComparisonOp::Equal,
        sql::IntLiteral{0, 0},
        0,
    };
}

bool is_literal_with_value(const plan::BoundScalarExpr& expression, std::int64_t expected) {
    const auto value = literal_value(expression);
    return value.has_value() && *value == expected;
}

bool is_canonical_true(const plan::BoundComparisonExpr& comparison) {
    return comparison.op == sql::ComparisonOp::Equal && is_literal_with_value(comparison.left, 1) &&
           is_literal_with_value(comparison.right, 1);
}

bool is_canonical_false(const plan::BoundComparisonExpr& comparison) {
    return comparison.op == sql::ComparisonOp::Equal && is_literal_with_value(comparison.left, 1) &&
           is_literal_with_value(comparison.right, 0);
}

std::optional<plan::LogicalPlan> rewrite_once(
    const plan::LogicalPlan& logical,
    const std::vector<std::reference_wrapper<const Rule>>& rules,
    RewriteTrace& trace) {
    switch (logical.kind) {
    case plan::LogicalKind::Project:
    case plan::LogicalKind::Filter: {
        const auto rewritten_child = rewrite_once(require_input(logical), rules, trace);
        if (rewritten_child.has_value()) {
            auto rewritten = logical;
            rewritten.input = std::make_shared<plan::LogicalPlan>(*rewritten_child);
            return rewritten;
        }
        break;
    }
    case plan::LogicalKind::Join: {
        const auto rewritten_left = rewrite_once(require_left(logical), rules, trace);
        if (rewritten_left.has_value()) {
            auto rewritten = logical;
            rewritten.left = std::make_shared<plan::LogicalPlan>(*rewritten_left);
            return rewritten;
        }

        const auto rewritten_right = rewrite_once(require_right(logical), rules, trace);
        if (rewritten_right.has_value()) {
            auto rewritten = logical;
            rewritten.right = std::make_shared<plan::LogicalPlan>(*rewritten_right);
            return rewritten;
        }
        break;
    }
    case plan::LogicalKind::Scan:
        break;
    }

    for (const auto& rule : rules) {
        auto rewritten = rule.get().apply(logical);
        if (rewritten.has_value()) {
            trace.fired_rules.push_back(std::string(rule.get().name()));
            return rewritten;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<plan::LogicalPlan> ConstantFoldComparisonRule::apply(const plan::LogicalPlan& logical) const {
    if (logical.kind != plan::LogicalKind::Filter) {
        return std::nullopt;
    }

    auto predicates = logical.predicates;
    bool changed = false;
    for (auto& predicate : predicates) {
        if (is_canonical_true(predicate) || is_canonical_false(predicate)) {
            continue;
        }

        const auto left = literal_value(predicate.left);
        const auto right = literal_value(predicate.right);
        if (!left.has_value() || !right.has_value()) {
            continue;
        }

        predicate = compare_values(*left, predicate.op, *right) ? canonical_true() : canonical_false();
        changed = true;
    }

    if (!changed) {
        return std::nullopt;
    }

    auto rewritten = logical;
    rewritten.predicates = std::move(predicates);
    return rewritten;
}

std::optional<plan::LogicalPlan> DropAlwaysTrueFilterRule::apply(const plan::LogicalPlan& logical) const {
    if (logical.kind != plan::LogicalKind::Filter) {
        return std::nullopt;
    }

    std::vector<plan::BoundComparisonExpr> predicates;
    predicates.reserve(logical.predicates.size());
    bool changed = false;
    for (const auto& predicate : logical.predicates) {
        if (is_canonical_true(predicate)) {
            changed = true;
            continue;
        }
        predicates.push_back(predicate);
    }

    if (!changed) {
        return std::nullopt;
    }

    if (predicates.empty()) {
        return require_input(logical);
    }

    return plan::LogicalPlan::filter(std::move(predicates), require_input(logical));
}

std::optional<plan::LogicalPlan> AlwaysFalseFilterRule::apply(const plan::LogicalPlan& logical) const {
    if (logical.kind != plan::LogicalKind::Filter) {
        return std::nullopt;
    }

    bool has_false = false;
    for (const auto& predicate : logical.predicates) {
        if (is_canonical_false(predicate)) {
            has_false = true;
            break;
        }
    }

    if (!has_false) {
        return std::nullopt;
    }
    if (logical.predicates.size() == 1 && is_canonical_false(logical.predicates.front())) {
        return std::nullopt;
    }

    return plan::LogicalPlan::filter({canonical_false()}, require_input(logical));
}

std::optional<plan::LogicalPlan> MergeAdjacentFiltersRule::apply(const plan::LogicalPlan& logical) const {
    if (logical.kind != plan::LogicalKind::Filter) {
        return std::nullopt;
    }

    const auto& child = require_input(logical);
    if (child.kind != plan::LogicalKind::Filter) {
        return std::nullopt;
    }

    std::vector<plan::BoundComparisonExpr> predicates;
    predicates.reserve(child.predicates.size() + logical.predicates.size());
    predicates.insert(predicates.end(), child.predicates.begin(), child.predicates.end());
    predicates.insert(predicates.end(), logical.predicates.begin(), logical.predicates.end());
    return plan::LogicalPlan::filter(std::move(predicates), require_input(child));
}

RewriteResult rewrite_to_fixpoint(const plan::LogicalPlan& logical,
                                  const std::vector<std::reference_wrapper<const Rule>>& rules,
                                  RewriteOptions options) {
    RewriteResult result;
    result.plan = logical;

    for (std::size_t pass = 0; pass < options.max_passes; ++pass) {
        auto rewritten = rewrite_once(result.plan, rules, result.trace);
        if (!rewritten.has_value()) {
            result.passes = pass;
            result.reached_fixpoint = true;
            return result;
        }
        result.plan = std::move(*rewritten);
    }

    throw std::logic_error("optimizer rewrite did not converge within max_passes");
}

std::vector<std::reference_wrapper<const Rule>> default_rules() {
    static const ConstantFoldComparisonRule constant_fold;
    static const MergeAdjacentFiltersRule merge_filters;
    static const AlwaysFalseFilterRule always_false;
    static const DropAlwaysTrueFilterRule drop_true;
    return {std::cref(constant_fold), std::cref(merge_filters), std::cref(always_false), std::cref(drop_true)};
}

} // namespace optimizer
