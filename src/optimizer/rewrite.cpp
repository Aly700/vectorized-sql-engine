#include "optimizer/rewrite.hpp"

#include <algorithm>
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
    if (const auto* literal = std::get_if<sql::IntLiteral>(&expression.value)) {
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

plan::BoundPredicate comparison_predicate(plan::BoundComparisonExpr comparison) {
    return plan::BoundPredicate::comparison_expr(std::move(comparison));
}

plan::BoundScalarExpr int_literal(std::int64_t value) {
    return plan::BoundScalarExpr{sql::IntLiteral{value, 0}, catalog::ColumnType::Int64};
}

plan::BoundPredicate canonical_true() {
    return comparison_predicate(plan::BoundComparisonExpr{
        int_literal(1),
        sql::ComparisonOp::Equal,
        int_literal(1),
        0,
    });
}

plan::BoundPredicate canonical_false() {
    return comparison_predicate(plan::BoundComparisonExpr{
        int_literal(1),
        sql::ComparisonOp::Equal,
        int_literal(0),
        0,
    });
}

bool is_literal_with_value(const plan::BoundScalarExpr& expression, std::int64_t expected) {
    const auto value = literal_value(expression);
    return value.has_value() && *value == expected;
}

bool is_canonical_true(const plan::BoundPredicate& predicate) {
    if (predicate.kind != sql::PredicateKind::Comparison) {
        return false;
    }
    const auto& comparison = predicate.comparison;
    return comparison.op == sql::ComparisonOp::Equal && is_literal_with_value(comparison.left, 1) &&
           is_literal_with_value(comparison.right, 1);
}

bool is_canonical_false(const plan::BoundPredicate& predicate) {
    if (predicate.kind != sql::PredicateKind::Comparison) {
        return false;
    }
    const auto& comparison = predicate.comparison;
    return comparison.op == sql::ComparisonOp::Equal && is_literal_with_value(comparison.left, 1) &&
           is_literal_with_value(comparison.right, 0);
}

MemoExpression filter_expression(std::vector<plan::BoundPredicate> predicates,
                                 GroupId child,
                                 plan::OrderPermission order_permission) {
    MemoExpression expression;
    expression.kind = MemoExpressionKind::Filter;
    expression.order_permission = order_permission;
    expression.predicates = std::move(predicates);
    expression.children.push_back(child);
    return expression;
}

MemoExpression join_expression(std::vector<plan::BoundPredicate> predicates,
                               GroupId left,
                               GroupId right,
                               plan::OrderPermission order_permission,
                               plan::JoinKind join_kind = plan::JoinKind::Inner) {
    MemoExpression expression;
    expression.kind = MemoExpressionKind::Join;
    expression.order_permission = order_permission;
    expression.join_kind = join_kind;
    expression.predicates = std::move(predicates);
    expression.children.push_back(left);
    expression.children.push_back(right);
    return expression;
}

MemoExpression aggregate_expression(std::vector<plan::BoundColumnRef> group_keys,
                                    std::vector<plan::AggregateExpression> aggregate_expressions,
                                    GroupId child,
                                    plan::OrderPermission order_permission) {
    MemoExpression expression;
    expression.kind = MemoExpressionKind::Aggregate;
    expression.order_permission = order_permission;
    expression.group_keys = std::move(group_keys);
    expression.aggregate_expressions = std::move(aggregate_expressions);
    expression.children.push_back(child);
    return expression;
}

plan::OrderPermission order_permission_for_group(const Memo& memo, GroupId group) {
    return memo.group(group).expressions.front().expression.order_permission;
}

void add_unique_table(std::vector<std::string>& tables, const std::string& table) {
    if (std::find(tables.begin(), tables.end(), table) == tables.end()) {
        tables.push_back(table);
        std::sort(tables.begin(), tables.end());
    }
}

std::vector<std::string> merge_tables(std::vector<std::string> left, const std::vector<std::string>& right) {
    for (const auto& table : right) {
        add_unique_table(left, table);
    }
    return left;
}

std::vector<std::string> referenced_tables(const plan::BoundScalarExpr& expression) {
    std::vector<std::string> tables;
    if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression.value)) {
        add_unique_table(tables, column->binding);
    }
    return tables;
}

std::vector<std::string> referenced_tables(const plan::BoundComparisonExpr& comparison) {
    return merge_tables(referenced_tables(comparison.left), referenced_tables(comparison.right));
}

std::vector<std::string> referenced_tables(const plan::BoundPredicate& predicate) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return referenced_tables(predicate.comparison);
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        return referenced_tables(predicate.null_check);
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("bound predicate is missing a child");
        }
        return merge_tables(referenced_tables(*predicate.left), referenced_tables(*predicate.right));
    }
    throw std::logic_error("unreachable predicate kind");
}

std::vector<plan::BoundColumnRef> referenced_columns(const plan::BoundScalarExpr& expression) {
    std::vector<plan::BoundColumnRef> columns;
    if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression.value)) {
        columns.push_back(*column);
    }
    return columns;
}

std::vector<plan::BoundColumnRef> referenced_columns(const plan::BoundComparisonExpr& comparison) {
    auto columns = referenced_columns(comparison.left);
    auto right_columns = referenced_columns(comparison.right);
    columns.insert(columns.end(), right_columns.begin(), right_columns.end());
    return columns;
}

std::vector<plan::BoundColumnRef> referenced_columns(const plan::BoundPredicate& predicate) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return referenced_columns(predicate.comparison);
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        return referenced_columns(predicate.null_check);
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or: {
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("bound predicate is missing a child");
        }
        auto columns = referenced_columns(*predicate.left);
        auto right_columns = referenced_columns(*predicate.right);
        columns.insert(columns.end(), right_columns.begin(), right_columns.end());
        return columns;
    }
    }
    throw std::logic_error("unreachable predicate kind");
}

bool contains_table(const std::vector<std::string>& tables, const std::string& table) {
    return std::find(tables.begin(), tables.end(), table) != tables.end();
}

bool is_subset(const std::vector<std::string>& candidate, const std::vector<std::string>& allowed) {
    for (const auto& table : candidate) {
        if (!contains_table(allowed, table)) {
            return false;
        }
    }
    return true;
}

bool references_side(const plan::BoundPredicate& predicate, const std::vector<std::string>& tables) {
    for (const auto& table : referenced_tables(predicate)) {
        if (contains_table(tables, table)) {
            return true;
        }
    }
    return false;
}

bool connects_children(const plan::BoundPredicate& predicate,
                       const std::vector<std::string>& left_tables,
                       const std::vector<std::string>& right_tables) {
    return references_side(predicate, left_tables) && references_side(predicate, right_tables);
}

bool has_connecting_predicate(const std::vector<plan::BoundPredicate>& predicates,
                              const std::vector<std::string>& left_tables,
                              const std::vector<std::string>& right_tables) {
    for (const auto& predicate : predicates) {
        if (connects_children(predicate, left_tables, right_tables)) {
            return true;
        }
    }
    return false;
}

bool bound_column_equal(const plan::BoundColumnRef& left, const plan::BoundColumnRef& right) {
    return left.binding == right.binding && left.column == right.column;
}

bool contains_group_key(const std::vector<plan::BoundColumnRef>& group_keys, const plan::BoundColumnRef& column) {
    for (const auto& key : group_keys) {
        if (bound_column_equal(key, column)) {
            return true;
        }
    }
    return false;
}

bool references_only_group_keys(const plan::BoundPredicate& predicate,
                                const std::vector<plan::BoundColumnRef>& group_keys) {
    const auto columns = referenced_columns(predicate);
    if (columns.empty()) {
        return false;
    }
    for (const auto& column : columns) {
        if (!contains_group_key(group_keys, column)) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> output_tables_for_group(const Memo& memo, GroupId group, std::vector<GroupId>& stack);

std::vector<std::string> output_tables_for_expression(const Memo& memo,
                                                      const MemoExpression& expression,
                                                      std::vector<GroupId>& stack) {
    switch (expression.kind) {
    case MemoExpressionKind::Scan:
        return {expression.binding_name};
    case MemoExpressionKind::Filter:
    case MemoExpressionKind::Project:
    case MemoExpressionKind::Aggregate:
    case MemoExpressionKind::Distinct:
    case MemoExpressionKind::Sort:
    case MemoExpressionKind::Limit:
    case MemoExpressionKind::GroupRef:
        return output_tables_for_group(memo, expression.children.at(0), stack);
    case MemoExpressionKind::Join:
        return merge_tables(output_tables_for_group(memo, expression.children.at(0), stack),
                            output_tables_for_group(memo, expression.children.at(1), stack));
    }
    throw std::logic_error("unreachable memo expression kind");
}

std::vector<std::string> output_tables_for_group(const Memo& memo, GroupId group, std::vector<GroupId>& stack) {
    group = memo.representative(group);
    if (std::find(stack.begin(), stack.end(), group) != stack.end()) {
        throw std::logic_error("memo table-set derivation encountered a cycle");
    }

    stack.push_back(group);
    const auto& memo_group = memo.group(group);
    auto tables = output_tables_for_expression(memo, memo_group.expressions.front().expression, stack);
    for (std::size_t i = 1; i < memo_group.expressions.size(); ++i) {
        const auto alternative_tables = output_tables_for_expression(memo, memo_group.expressions[i].expression, stack);
        if (alternative_tables != tables) {
            throw std::logic_error("memo group alternatives expose different binding identity sets");
        }
    }
    stack.pop_back();
    return tables;
}

std::vector<std::string> output_tables_for_group(const Memo& memo, GroupId group) {
    std::vector<GroupId> stack;
    return output_tables_for_group(memo, group, stack);
}

bool can_apply_join_transform(const MemoExpression& expression) {
    return expression.kind == MemoExpressionKind::Join &&
           expression.join_kind == plan::JoinKind::Inner &&
           expression.order_permission == plan::OrderPermission::Arbitrary;
}

std::optional<plan::LogicalPlan> rewrite_once(
    const plan::LogicalPlan& logical,
    const std::vector<std::reference_wrapper<const Rule>>& rules,
    RewriteTrace& trace) {
    switch (logical.kind) {
    case plan::LogicalKind::Project:
    case plan::LogicalKind::Aggregate:
    case plan::LogicalKind::Distinct:
    case plan::LogicalKind::Sort:
    case plan::LogicalKind::Limit:
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
    case plan::LogicalKind::Explain:
        throw std::invalid_argument("EXPLAIN logical plans cannot be rewritten directly");
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

plan::BoundPredicate fold_predicate_tree(const plan::BoundPredicate& predicate, bool& changed) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison: {
        if (is_canonical_true(predicate) || is_canonical_false(predicate)) {
            return predicate;
        }

        const auto left = literal_value(predicate.comparison.left);
        const auto right = literal_value(predicate.comparison.right);
        if (!left.has_value() || !right.has_value()) {
            return predicate;
        }

        changed = true;
        return compare_values(*left, predicate.comparison.op, *right) ? canonical_true() : canonical_false();
    }
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        return predicate;
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("bound predicate is missing a child");
        }
        break;
    }

    auto left = fold_predicate_tree(*predicate.left, changed);
    auto right = fold_predicate_tree(*predicate.right, changed);

    if (predicate.kind == sql::PredicateKind::And) {
        if (is_canonical_false(left) || is_canonical_false(right)) {
            changed = true;
            return canonical_false();
        }
        if (is_canonical_true(left)) {
            changed = true;
            return right;
        }
        if (is_canonical_true(right)) {
            changed = true;
            return left;
        }
        return plan::BoundPredicate::binary(sql::PredicateKind::And,
                                            std::move(left),
                                            std::move(right),
                                            predicate.operator_position);
    }

    if (is_canonical_true(left) || is_canonical_true(right)) {
        changed = true;
        return canonical_true();
    }
    if (is_canonical_false(left)) {
        changed = true;
        return right;
    }
    if (is_canonical_false(right)) {
        changed = true;
        return left;
    }
    return plan::BoundPredicate::binary(sql::PredicateKind::Or,
                                        std::move(left),
                                        std::move(right),
                                        predicate.operator_position);
}

} // namespace

std::optional<plan::LogicalPlan> ConstantFoldComparisonRule::apply(const plan::LogicalPlan& logical) const {
    if (logical.kind != plan::LogicalKind::Filter) {
        return std::nullopt;
    }

    auto predicates = logical.predicates;
    bool changed = false;
    for (auto& predicate : predicates) {
        predicate = fold_predicate_tree(predicate, changed);
    }

    if (!changed) {
        return std::nullopt;
    }

    auto rewritten = logical;
    rewritten.predicates = std::move(predicates);
    return rewritten;
}

bool ConstantFoldComparisonRule::apply(Memo& memo, GroupId group, const MemoExpression& expression) const {
    if (expression.kind != MemoExpressionKind::Filter) {
        return false;
    }

    auto predicates = expression.predicates;
    bool changed = false;
    for (auto& predicate : predicates) {
        predicate = fold_predicate_tree(predicate, changed);
    }

    if (!changed) {
        return false;
    }

    return memo.insert_equivalent(
        group,
        filter_expression(std::move(predicates), expression.children.at(0), expression.order_permission));
}

std::optional<plan::LogicalPlan> DropAlwaysTrueFilterRule::apply(const plan::LogicalPlan& logical) const {
    if (logical.kind != plan::LogicalKind::Filter) {
        return std::nullopt;
    }

    std::vector<plan::BoundPredicate> predicates;
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

    auto rewritten = plan::LogicalPlan::filter(std::move(predicates), require_input(logical));
    rewritten.order_permission = logical.order_permission;
    return rewritten;
}

bool DropAlwaysTrueFilterRule::apply(Memo& memo, GroupId group, const MemoExpression& expression) const {
    if (expression.kind != MemoExpressionKind::Filter) {
        return false;
    }

    std::vector<plan::BoundPredicate> predicates;
    predicates.reserve(expression.predicates.size());
    bool changed = false;
    for (const auto& predicate : expression.predicates) {
        if (is_canonical_true(predicate)) {
            changed = true;
            continue;
        }
        predicates.push_back(predicate);
    }

    if (!changed) {
        return false;
    }

    if (predicates.empty()) {
        return memo.insert_group_ref_equivalent(group, expression.children.at(0));
    }

    return memo.insert_equivalent(
        group,
        filter_expression(std::move(predicates), expression.children.at(0), expression.order_permission));
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

    auto rewritten = plan::LogicalPlan::filter({canonical_false()}, require_input(logical));
    rewritten.order_permission = logical.order_permission;
    return rewritten;
}

bool AlwaysFalseFilterRule::apply(Memo& memo, GroupId group, const MemoExpression& expression) const {
    if (expression.kind != MemoExpressionKind::Filter) {
        return false;
    }

    bool has_false = false;
    for (const auto& predicate : expression.predicates) {
        if (is_canonical_false(predicate)) {
            has_false = true;
            break;
        }
    }

    if (!has_false) {
        return false;
    }
    if (expression.predicates.size() == 1 && is_canonical_false(expression.predicates.front())) {
        return false;
    }

    return memo.insert_equivalent(group,
                                  filter_expression({canonical_false()},
                                                    expression.children.at(0),
                                                    expression.order_permission));
}

std::optional<plan::LogicalPlan> MergeAdjacentFiltersRule::apply(const plan::LogicalPlan& logical) const {
    if (logical.kind != plan::LogicalKind::Filter) {
        return std::nullopt;
    }

    const auto& child = require_input(logical);
    if (child.kind != plan::LogicalKind::Filter) {
        return std::nullopt;
    }

    std::vector<plan::BoundPredicate> predicates;
    predicates.reserve(child.predicates.size() + logical.predicates.size());
    predicates.insert(predicates.end(), child.predicates.begin(), child.predicates.end());
    predicates.insert(predicates.end(), logical.predicates.begin(), logical.predicates.end());
    auto rewritten = plan::LogicalPlan::filter(std::move(predicates), require_input(child));
    rewritten.order_permission = logical.order_permission;
    return rewritten;
}

bool MergeAdjacentFiltersRule::apply(Memo& memo, GroupId group, const MemoExpression& expression) const {
    if (expression.kind != MemoExpressionKind::Filter) {
        return false;
    }

    bool changed = false;
    const auto child_group = expression.children.at(0);
    const auto child_expression_count = memo.group(child_group).expressions.size();
    for (std::size_t i = 0; i < child_expression_count; ++i) {
        const auto child_expression = memo.group(child_group).expressions.at(i).expression;
        if (child_expression.kind != MemoExpressionKind::Filter) {
            continue;
        }

        std::vector<plan::BoundPredicate> predicates;
        predicates.reserve(child_expression.predicates.size() + expression.predicates.size());
        predicates.insert(predicates.end(), child_expression.predicates.begin(), child_expression.predicates.end());
        predicates.insert(predicates.end(), expression.predicates.begin(), expression.predicates.end());
        changed = memo.insert_equivalent(
                      group,
                      filter_expression(std::move(predicates),
                                        child_expression.children.at(0),
                                        expression.order_permission)) ||
                  changed;
    }

    return changed;
}

bool FilterIntoJoinRule::apply(Memo& memo, GroupId group, const MemoExpression& expression) const {
    if (expression.kind != MemoExpressionKind::Filter) {
        return false;
    }

    const auto child_group = expression.children.at(0);
    bool changed = false;
    const auto child_expression_count = memo.group(child_group).expressions.size();
    for (std::size_t i = 0; i < child_expression_count; ++i) {
        const auto child_expression = memo.group(child_group).expressions.at(i).expression;
        if (child_expression.kind != MemoExpressionKind::Join ||
            child_expression.join_kind != plan::JoinKind::Inner) {
            continue;
        }

        const auto left_group = child_expression.children.at(0);
        const auto right_group = child_expression.children.at(1);
        const auto left_tables = output_tables_for_group(memo, left_group);
        const auto right_tables = output_tables_for_group(memo, right_group);
        const auto join_tables = merge_tables(left_tables, right_tables);

        std::vector<plan::BoundPredicate> left_predicates;
        std::vector<plan::BoundPredicate> right_predicates;
        std::vector<plan::BoundPredicate> join_predicates = child_expression.predicates;
        std::vector<plan::BoundPredicate> residual_predicates;
        bool moved = false;
        for (const auto& predicate : expression.predicates) {
            const auto refs = referenced_tables(predicate);
            if (refs.empty()) {
                residual_predicates.push_back(predicate);
                continue;
            }

            const auto left_only = is_subset(refs, left_tables);
            const auto right_only = is_subset(refs, right_tables);
            if (left_only && !right_only) {
                left_predicates.push_back(predicate);
                moved = true;
                continue;
            }
            if (right_only && !left_only) {
                right_predicates.push_back(predicate);
                moved = true;
                continue;
            }
            if (predicate.kind == sql::PredicateKind::Comparison && is_subset(refs, join_tables) &&
                connects_children(predicate, left_tables, right_tables)) {
                join_predicates.push_back(predicate);
                moved = true;
                continue;
            }

            residual_predicates.push_back(predicate);
        }

        if (!moved) {
            continue;
        }

        const auto before_group_count = memo.group_count();
        auto pushed_left_group = left_group;
        if (!left_predicates.empty()) {
            pushed_left_group = memo.insert_expression(filter_expression(std::move(left_predicates),
                                                                         left_group,
                                                                         order_permission_for_group(memo, left_group)));
        }

        auto pushed_right_group = right_group;
        if (!right_predicates.empty()) {
            pushed_right_group = memo.insert_expression(filter_expression(std::move(right_predicates),
                                                                          right_group,
                                                                          order_permission_for_group(memo, right_group)));
        }

        auto pushed_join = join_expression(std::move(join_predicates),
                                           pushed_left_group,
                                           pushed_right_group,
                                           expression.order_permission);
        const auto pushed_join_group = memo.insert_expression(std::move(pushed_join));
        if (residual_predicates.empty()) {
            const auto inserted = memo.insert_group_ref_equivalent(group, pushed_join_group);
            changed = inserted || memo.group_count() != before_group_count || changed;
            continue;
        }

        const auto inserted = memo.insert_equivalent(group,
                                                     filter_expression(std::move(residual_predicates),
                                                                       pushed_join_group,
                                                                       expression.order_permission));
        changed = inserted || memo.group_count() != before_group_count || changed;
    }

    return changed;
}

bool FilterThroughAggregateRule::apply(Memo& memo, GroupId group, const MemoExpression& expression) const {
    if (expression.kind != MemoExpressionKind::Filter) {
        return false;
    }

    const auto child_group = expression.children.at(0);
    bool changed = false;
    const auto child_expression_count = memo.group(child_group).expressions.size();
    for (std::size_t i = 0; i < child_expression_count; ++i) {
        const auto child_expression = memo.group(child_group).expressions.at(i).expression;
        if (child_expression.kind != MemoExpressionKind::Aggregate) {
            continue;
        }

        std::vector<plan::BoundPredicate> pushable_predicates;
        std::vector<plan::BoundPredicate> residual_predicates;
        for (const auto& predicate : expression.predicates) {
            if (references_only_group_keys(predicate, child_expression.group_keys)) {
                pushable_predicates.push_back(predicate);
            } else {
                residual_predicates.push_back(predicate);
            }
        }

        if (pushable_predicates.empty()) {
            continue;
        }

        const auto before_group_count = memo.group_count();
        const auto aggregate_input_group = child_expression.children.at(0);
        const auto filtered_input_group =
            memo.insert_expression(filter_expression(std::move(pushable_predicates),
                                                     aggregate_input_group,
                                                     order_permission_for_group(memo, aggregate_input_group)));
        auto pushed_aggregate = aggregate_expression(child_expression.group_keys,
                                                     child_expression.aggregate_expressions,
                                                     filtered_input_group,
                                                     expression.order_permission);
        const auto pushed_aggregate_group = memo.insert_expression(std::move(pushed_aggregate));
        if (residual_predicates.empty()) {
            const auto inserted = memo.insert_group_ref_equivalent(group, pushed_aggregate_group);
            changed = inserted || memo.group_count() != before_group_count || changed;
            continue;
        }

        const auto inserted = memo.insert_equivalent(group,
                                                     filter_expression(std::move(residual_predicates),
                                                                       pushed_aggregate_group,
                                                                       expression.order_permission));
        changed = inserted || memo.group_count() != before_group_count || changed;
    }

    return changed;
}

bool JoinCommuteRule::apply(Memo& memo, GroupId group, const MemoExpression& expression) const {
    if (!can_apply_join_transform(expression)) {
        return false;
    }

    return memo.insert_equivalent(
        group,
        join_expression(expression.predicates,
                        expression.children.at(1),
                        expression.children.at(0),
                        expression.order_permission));
}

namespace {

bool try_left_to_right_associate(Memo& memo, GroupId group, const MemoExpression& expression) {
    const auto left_group = expression.children.at(0);
    const auto right_group = expression.children.at(1);
    bool changed = false;

    const auto left_expression_count = memo.group(left_group).expressions.size();
    for (std::size_t i = 0; i < left_expression_count; ++i) {
        const auto left_expression = memo.group(left_group).expressions.at(i).expression;
        if (left_expression.kind != MemoExpressionKind::Join ||
            left_expression.join_kind != plan::JoinKind::Inner ||
            left_expression.order_permission != plan::OrderPermission::Arbitrary) {
            continue;
        }

        const auto a_group = left_expression.children.at(0);
        const auto b_group = left_expression.children.at(1);
        const auto a_tables = output_tables_for_group(memo, a_group);
        const auto b_tables = output_tables_for_group(memo, b_group);
        const auto c_tables = output_tables_for_group(memo, right_group);
        const auto inner_tables = merge_tables(b_tables, c_tables);
        const auto outer_tables = merge_tables(a_tables, inner_tables);

        std::vector<plan::BoundPredicate> inner_predicates;
        std::vector<plan::BoundPredicate> outer_predicates = left_expression.predicates;
        bool legal = true;
        for (const auto& predicate : expression.predicates) {
            const auto refs = referenced_tables(predicate);
            if (is_subset(refs, inner_tables)) {
                inner_predicates.push_back(predicate);
            } else if (is_subset(refs, outer_tables)) {
                outer_predicates.push_back(predicate);
            } else {
                legal = false;
                break;
            }
        }
        if (!legal || !has_connecting_predicate(inner_predicates, b_tables, c_tables)) {
            continue;
        }

        const auto before_group_count = memo.group_count();
        const auto inner_group = memo.insert_expression(join_expression(std::move(inner_predicates),
                                                                        b_group,
                                                                        right_group,
                                                                        expression.order_permission));
        if (memo.representative(inner_group) == memo.representative(group)) {
            continue;
        }

        const auto inserted = memo.insert_equivalent(group,
                                                     join_expression(std::move(outer_predicates),
                                                                     a_group,
                                                                     inner_group,
                                                                     expression.order_permission));
        changed = inserted || memo.group_count() != before_group_count || changed;
    }

    return changed;
}

bool try_right_to_left_associate(Memo& memo, GroupId group, const MemoExpression& expression) {
    const auto left_group = expression.children.at(0);
    const auto right_group = expression.children.at(1);
    bool changed = false;

    const auto right_expression_count = memo.group(right_group).expressions.size();
    for (std::size_t i = 0; i < right_expression_count; ++i) {
        const auto right_expression = memo.group(right_group).expressions.at(i).expression;
        if (right_expression.kind != MemoExpressionKind::Join ||
            right_expression.join_kind != plan::JoinKind::Inner ||
            right_expression.order_permission != plan::OrderPermission::Arbitrary) {
            continue;
        }

        const auto b_group = right_expression.children.at(0);
        const auto c_group = right_expression.children.at(1);
        const auto a_tables = output_tables_for_group(memo, left_group);
        const auto b_tables = output_tables_for_group(memo, b_group);
        const auto c_tables = output_tables_for_group(memo, c_group);
        const auto inner_tables = merge_tables(a_tables, b_tables);
        const auto outer_tables = merge_tables(inner_tables, c_tables);

        std::vector<plan::BoundPredicate> inner_predicates;
        std::vector<plan::BoundPredicate> residual_outer_predicates;
        bool legal = true;
        for (const auto& predicate : expression.predicates) {
            const auto refs = referenced_tables(predicate);
            if (is_subset(refs, inner_tables)) {
                inner_predicates.push_back(predicate);
            } else if (is_subset(refs, outer_tables)) {
                residual_outer_predicates.push_back(predicate);
            } else {
                legal = false;
                break;
            }
        }
        if (!legal || !has_connecting_predicate(inner_predicates, a_tables, b_tables)) {
            continue;
        }

        std::vector<plan::BoundPredicate> outer_predicates = right_expression.predicates;
        outer_predicates.insert(outer_predicates.end(),
                                residual_outer_predicates.begin(),
                                residual_outer_predicates.end());

        const auto before_group_count = memo.group_count();
        const auto inner_group = memo.insert_expression(join_expression(std::move(inner_predicates),
                                                                        left_group,
                                                                        b_group,
                                                                        expression.order_permission));
        if (memo.representative(inner_group) == memo.representative(group)) {
            continue;
        }

        const auto inserted = memo.insert_equivalent(group,
                                                     join_expression(std::move(outer_predicates),
                                                                     inner_group,
                                                                     c_group,
                                                                     expression.order_permission));
        changed = inserted || memo.group_count() != before_group_count || changed;
    }

    return changed;
}

} // namespace

bool JoinAssociateRule::apply(Memo& memo, GroupId group, const MemoExpression& expression) const {
    if (!can_apply_join_transform(expression)) {
        return false;
    }

    const auto left_changed = try_left_to_right_associate(memo, group, expression);
    const auto right_changed = try_right_to_left_associate(memo, group, expression);
    return left_changed || right_changed;
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

MemoExploreResult explore_memo_to_fixpoint(Memo& memo,
                                           const std::vector<std::reference_wrapper<const MemoRule>>& rules,
                                           MemoExploreOptions options) {
    MemoExploreResult result;
    for (std::size_t iteration = 0; iteration < options.max_iterations; ++iteration) {
        bool changed = false;
        const auto group_count = memo.group_count();
        for (GroupId group_id = 1; group_id <= group_count; ++group_id) {
            const auto expression_count = memo.group(group_id).expressions.size();
            for (std::size_t expression_index = 0; expression_index < expression_count; ++expression_index) {
                const auto expression = memo.group(group_id).expressions.at(expression_index).expression;
                for (const auto& rule : rules) {
                    if (rule.get().apply(memo, group_id, expression)) {
                        changed = true;
                        result.fired_rules.push_back(std::string(rule.get().name()));
                        memo.assert_invariants();
                    }
                }
            }
        }

        if (!changed) {
            result.iterations = iteration;
            result.reached_fixpoint = true;
            return result;
        }
    }

    throw std::logic_error("optimizer memo exploration did not converge within max_iterations");
}

std::vector<std::reference_wrapper<const MemoRule>> default_memo_rules() {
    static const ConstantFoldComparisonRule constant_fold;
    static const MergeAdjacentFiltersRule merge_filters;
    static const AlwaysFalseFilterRule always_false;
    static const DropAlwaysTrueFilterRule drop_true;
    static const FilterIntoJoinRule filter_into_join;
    static const FilterThroughAggregateRule filter_through_aggregate;
    static const JoinCommuteRule join_commute;
    static const JoinAssociateRule join_associate;
    return {std::cref(static_cast<const MemoRule&>(constant_fold)),
            std::cref(static_cast<const MemoRule&>(merge_filters)),
            std::cref(static_cast<const MemoRule&>(always_false)),
            std::cref(static_cast<const MemoRule&>(drop_true)),
            std::cref(filter_into_join),
            std::cref(filter_through_aggregate),
            std::cref(join_commute),
            std::cref(join_associate)};
}

} // namespace optimizer
