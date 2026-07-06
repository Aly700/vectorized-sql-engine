#include "optimizer/memo.hpp"

#include <algorithm>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <variant>

namespace optimizer {
namespace {

void hash_combine(std::size_t& seed, std::size_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
}

void hash_string(std::size_t& seed, const std::string& value) {
    hash_combine(seed, std::hash<std::string>{}(value));
}

void hash_scalar(std::size_t& seed, const plan::BoundScalarExpr& expression) {
    if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression)) {
        hash_combine(seed, 1);
        hash_string(seed, column->table);
        hash_string(seed, column->column);
        return;
    }

    hash_combine(seed, 2);
    hash_combine(seed, std::hash<std::int64_t>{}(std::get<sql::IntLiteral>(expression).value));
}

void hash_comparison(std::size_t& seed, const plan::BoundComparisonExpr& comparison) {
    hash_scalar(seed, comparison.left);
    hash_combine(seed, static_cast<std::size_t>(comparison.op));
    hash_scalar(seed, comparison.right);
}

void hash_projection(std::size_t& seed, const plan::Projection& projection) {
    hash_string(seed, projection.output_name);
    hash_scalar(seed, projection.expression);
}

std::size_t structural_hash(const MemoExpression& expression) {
    std::size_t seed = 0;
    hash_combine(seed, static_cast<std::size_t>(expression.kind));
    hash_string(seed, expression.table);

    hash_combine(seed, expression.projections.size());
    for (const auto& projection : expression.projections) {
        hash_projection(seed, projection);
    }

    hash_combine(seed, expression.predicates.size());
    for (const auto& predicate : expression.predicates) {
        hash_comparison(seed, predicate);
    }

    hash_combine(seed, expression.children.size());
    for (const auto child : expression.children) {
        hash_combine(seed, static_cast<std::size_t>(child));
    }

    return seed;
}

bool scalar_equal(const plan::BoundScalarExpr& left, const plan::BoundScalarExpr& right) {
    if (left.index() != right.index()) {
        return false;
    }
    if (const auto* left_column = std::get_if<plan::BoundColumnRef>(&left)) {
        const auto& right_column = std::get<plan::BoundColumnRef>(right);
        return left_column->table == right_column.table && left_column->column == right_column.column;
    }
    return std::get<sql::IntLiteral>(left).value == std::get<sql::IntLiteral>(right).value;
}

bool comparison_equal(const plan::BoundComparisonExpr& left, const plan::BoundComparisonExpr& right) {
    return scalar_equal(left.left, right.left) && left.op == right.op && scalar_equal(left.right, right.right);
}

bool projection_equal(const plan::Projection& left, const plan::Projection& right) {
    return left.output_name == right.output_name && scalar_equal(left.expression, right.expression);
}

template <typename T, typename Equal>
bool vector_equal(const std::vector<T>& left, const std::vector<T>& right, Equal equal) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (!equal(left[i], right[i])) {
            return false;
        }
    }
    return true;
}

bool structural_equal(const MemoExpression& left, const MemoExpression& right) {
    return left.kind == right.kind && left.table == right.table &&
           vector_equal(left.projections, right.projections, projection_equal) &&
           vector_equal(left.predicates, right.predicates, comparison_equal) && left.children == right.children;
}

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

std::string expression_to_string(const plan::BoundScalarExpr& expression) {
    if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression)) {
        return "col(" + column->table + "." + column->column + ")";
    }
    return "lit(" + std::to_string(std::get<sql::IntLiteral>(expression).value) + ")";
}

std::string comparison_op_to_string(sql::ComparisonOp op) {
    switch (op) {
    case sql::ComparisonOp::Equal:
        return "=";
    case sql::ComparisonOp::NotEqual:
        return "<>";
    case sql::ComparisonOp::Less:
        return "<";
    case sql::ComparisonOp::LessEqual:
        return "<=";
    case sql::ComparisonOp::Greater:
        return ">";
    case sql::ComparisonOp::GreaterEqual:
        return ">=";
    }
    throw std::logic_error("unreachable comparison operator");
}

std::string comparison_to_string(const plan::BoundComparisonExpr& comparison) {
    return expression_to_string(comparison.left) + " " + comparison_op_to_string(comparison.op) + " " +
           expression_to_string(comparison.right);
}

void append_predicates(std::ostringstream& out, const std::vector<plan::BoundComparisonExpr>& predicates) {
    for (std::size_t i = 0; i < predicates.size(); ++i) {
        if (i != 0) {
            out << " AND ";
        }
        out << comparison_to_string(predicates[i]);
    }
}

void append_children(std::ostringstream& out, const std::vector<GroupId>& children) {
    out << " children=[";
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << children[i];
    }
    out << "]";
}

std::string memo_expression_to_string(const MemoExpression& expression) {
    std::ostringstream out;
    switch (expression.kind) {
    case MemoExpressionKind::Scan:
        out << "Scan[" << expression.table << "]";
        return out.str();
    case MemoExpressionKind::Join:
        out << "Join[";
        append_predicates(out, expression.predicates);
        out << "]";
        append_children(out, expression.children);
        return out.str();
    case MemoExpressionKind::Filter:
        out << "Filter[";
        append_predicates(out, expression.predicates);
        out << "]";
        append_children(out, expression.children);
        return out.str();
    case MemoExpressionKind::Project:
        out << "Project[";
        for (std::size_t i = 0; i < expression.projections.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << expression.projections[i].output_name << "="
                << expression_to_string(expression.projections[i].expression);
        }
        out << "]";
        append_children(out, expression.children);
        return out.str();
    case MemoExpressionKind::GroupRef:
        out << "GroupRef[" << expression.children.front() << "]";
        return out.str();
    }
    throw std::logic_error("unreachable memo expression kind");
}

} // namespace

GroupId Memo::insert(const plan::LogicalPlan& logical) {
    MemoExpression expression;
    switch (logical.kind) {
    case plan::LogicalKind::Scan:
        expression.kind = MemoExpressionKind::Scan;
        expression.table = logical.table;
        return insert_expression(std::move(expression));
    case plan::LogicalKind::Filter:
        expression.kind = MemoExpressionKind::Filter;
        expression.predicates = logical.predicates;
        expression.children.push_back(insert(require_input(logical)));
        return insert_expression(std::move(expression));
    case plan::LogicalKind::Project:
        expression.kind = MemoExpressionKind::Project;
        expression.projections = logical.projections;
        expression.children.push_back(insert(require_input(logical)));
        return insert_expression(std::move(expression));
    case plan::LogicalKind::Join:
        expression.kind = MemoExpressionKind::Join;
        expression.predicates = logical.predicates;
        expression.children.push_back(insert(require_left(logical)));
        expression.children.push_back(insert(require_right(logical)));
        return insert_expression(std::move(expression));
    }
    throw std::logic_error("unreachable logical plan kind");
}

GroupId Memo::insert_expression(MemoExpression expression) {
    validate_expression(expression);
    if (const auto existing = find_existing(expression); existing != 0) {
        return existing;
    }

    const auto id = static_cast<GroupId>(groups_.size() + 1);
    groups_.push_back(MemoGroup{id, {GroupExpression{std::move(expression)}}});
    index_expression(groups_.back().expressions.front().expression, id);
    assert_invariants();
    return id;
}

bool Memo::insert_equivalent(GroupId group_id, MemoExpression expression) {
    if (!has_group(group_id)) {
        throw std::out_of_range("unknown memo group");
    }
    validate_expression(expression);
    validate_no_cycle_from(group_id, expression);

    if (const auto existing = find_existing(expression); existing != 0) {
        if (existing == group_id) {
            return false;
        }
        throw std::logic_error("memo expression already belongs to a different group; use GroupRef for equivalence");
    }

    auto& target = groups_.at(index_for(group_id));
    target.expressions.push_back(GroupExpression{std::move(expression)});
    index_expression(target.expressions.back().expression, group_id);
    assert_invariants();
    return true;
}

bool Memo::insert_group_ref_equivalent(GroupId group_id, GroupId equivalent_group) {
    if (!has_group(group_id) || !has_group(equivalent_group)) {
        throw std::out_of_range("unknown memo group");
    }
    if (group_id == equivalent_group) {
        return false;
    }

    MemoExpression reference;
    reference.kind = MemoExpressionKind::GroupRef;
    reference.children.push_back(equivalent_group);
    return insert_equivalent(group_id, std::move(reference));
}

const MemoGroup& Memo::group(GroupId id) const {
    return groups_.at(index_for(id));
}

plan::LogicalPlan Memo::extract(GroupId id) const {
    assert_invariants();
    std::vector<GroupId> stack;
    return extract(id, stack);
}

std::string Memo::dump() const {
    assert_invariants();
    std::ostringstream out;
    for (const auto& memo_group : groups_) {
        out << "group " << memo_group.id << ":\n";
        for (std::size_t i = 0; i < memo_group.expressions.size(); ++i) {
            out << "  expr " << i << ": " << memo_expression_to_string(memo_group.expressions[i].expression) << "\n";
        }
    }
    return out.str();
}

void Memo::assert_invariants() const {
    std::size_t expression_count = 0;
    for (std::size_t i = 0; i < groups_.size(); ++i) {
        const auto expected_id = static_cast<GroupId>(i + 1);
        if (groups_[i].id != expected_id) {
            throw std::logic_error("memo group id does not match deterministic vector position");
        }
        if (groups_[i].expressions.empty()) {
            throw std::logic_error("memo group has no expressions");
        }

        for (const auto& expression : groups_[i].expressions) {
            validate_expression(expression.expression);
            validate_no_cycle_from(groups_[i].id, expression.expression);
            const auto owner = find_existing(expression.expression);
            if (owner != groups_[i].id) {
                throw std::logic_error("memo structural index does not point back to expression owner");
            }
            ++expression_count;
        }
    }

    std::size_t indexed_count = 0;
    for (const auto& bucket : expression_index_) {
        for (const auto& indexed : bucket.second) {
            if (structural_hash(indexed.first) != bucket.first) {
                throw std::logic_error("memo structural index bucket hash is inconsistent");
            }
            const auto& owner = group(indexed.second);
            const auto found = std::find_if(owner.expressions.begin(), owner.expressions.end(), [&](const auto& item) {
                return structural_equal(item.expression, indexed.first);
            });
            if (found == owner.expressions.end()) {
                throw std::logic_error("memo structural index references a missing expression");
            }
            ++indexed_count;
        }
    }

    if (indexed_count != expression_count) {
        throw std::logic_error("memo structural index cardinality does not match group expressions");
    }
}

bool Memo::has_group(GroupId id) const {
    return id != 0 && id <= groups_.size();
}

std::size_t Memo::index_for(GroupId id) const {
    if (!has_group(id)) {
        throw std::out_of_range("unknown memo group");
    }
    return static_cast<std::size_t>(id - 1);
}

GroupId Memo::find_existing(const MemoExpression& expression) const {
    const auto hash = structural_hash(expression);
    const auto it = expression_index_.find(hash);
    if (it == expression_index_.end()) {
        return 0;
    }
    for (const auto& candidate : it->second) {
        if (structural_equal(candidate.first, expression)) {
            return candidate.second;
        }
    }
    return 0;
}

bool Memo::reaches(GroupId from, GroupId target) const {
    std::vector<GroupId> stack;
    return reaches(from, target, stack);
}

bool Memo::reaches(GroupId from, GroupId target, std::vector<GroupId>& stack) const {
    if (from == target) {
        return true;
    }
    if (!has_group(from)) {
        throw std::out_of_range("unknown memo group");
    }
    if (std::find(stack.begin(), stack.end(), from) != stack.end()) {
        return false;
    }

    stack.push_back(from);
    for (const auto& expression : group(from).expressions) {
        for (const auto child : expression.expression.children) {
            if (reaches(child, target, stack)) {
                stack.pop_back();
                return true;
            }
        }
    }
    stack.pop_back();
    return false;
}

plan::LogicalPlan Memo::extract(GroupId id, std::vector<GroupId>& stack) const {
    const auto& memo_group = group(id);
    if (std::find(stack.begin(), stack.end(), id) != stack.end()) {
        throw std::logic_error("memo extraction encountered a cycle");
    }

    stack.push_back(id);
    const auto& expression = memo_group.expressions.front().expression;
    switch (expression.kind) {
    case MemoExpressionKind::Scan: {
        auto result = plan::LogicalPlan::scan(expression.table);
        stack.pop_back();
        return result;
    }
    case MemoExpressionKind::Filter: {
        auto result = plan::LogicalPlan::filter(expression.predicates, extract(expression.children.at(0), stack));
        stack.pop_back();
        return result;
    }
    case MemoExpressionKind::Project: {
        auto result = plan::LogicalPlan::project(expression.projections, extract(expression.children.at(0), stack));
        stack.pop_back();
        return result;
    }
    case MemoExpressionKind::Join: {
        auto result = plan::LogicalPlan::join(expression.predicates,
                                             extract(expression.children.at(0), stack),
                                             extract(expression.children.at(1), stack));
        stack.pop_back();
        return result;
    }
    case MemoExpressionKind::GroupRef: {
        auto result = extract(expression.children.at(0), stack);
        stack.pop_back();
        return result;
    }
    }
    throw std::logic_error("unreachable memo expression kind");
}

void Memo::validate_expression(const MemoExpression& expression) const {
    for (const auto child : expression.children) {
        if (!has_group(child)) {
            throw std::logic_error("memo expression references an unknown child group");
        }
    }

    switch (expression.kind) {
    case MemoExpressionKind::Scan:
        if (expression.table.empty() || !expression.projections.empty() || !expression.predicates.empty() ||
            !expression.children.empty()) {
            throw std::logic_error("malformed memo scan expression");
        }
        return;
    case MemoExpressionKind::Filter:
        if (!expression.table.empty() || !expression.projections.empty() || expression.predicates.empty() ||
            expression.children.size() != 1) {
            throw std::logic_error("malformed memo filter expression");
        }
        return;
    case MemoExpressionKind::Project:
        if (!expression.table.empty() || expression.projections.empty() || !expression.predicates.empty() ||
            expression.children.size() != 1) {
            throw std::logic_error("malformed memo project expression");
        }
        return;
    case MemoExpressionKind::Join:
        if (!expression.table.empty() || !expression.projections.empty() || expression.children.size() != 2) {
            throw std::logic_error("malformed memo join expression");
        }
        return;
    case MemoExpressionKind::GroupRef:
        if (!expression.table.empty() || !expression.projections.empty() || !expression.predicates.empty() ||
            expression.children.size() != 1) {
            throw std::logic_error("malformed memo group reference expression");
        }
        return;
    }
    throw std::logic_error("unreachable memo expression kind");
}

void Memo::validate_no_cycle_from(GroupId group_id, const MemoExpression& expression) const {
    for (const auto child : expression.children) {
        if (child == group_id || reaches(child, group_id)) {
            throw std::logic_error("memo expression would create a cycle among groups");
        }
    }
}

void Memo::index_expression(const MemoExpression& expression, GroupId owner) {
    expression_index_[structural_hash(expression)].push_back({expression, owner});
}

} // namespace optimizer
