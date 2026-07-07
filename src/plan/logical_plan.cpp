#include "plan/logical_plan.hpp"

#include <sstream>
#include <stdexcept>
#include <variant>

namespace plan {
namespace {

const LogicalPlan& require_input(const LogicalPlan& logical) {
    if (!logical.input) {
        throw std::invalid_argument("logical plan node is missing its input");
    }
    return *logical.input;
}

const LogicalPlan& require_left(const LogicalPlan& logical) {
    if (!logical.left) {
        throw std::invalid_argument("logical join node is missing its left input");
    }
    return *logical.left;
}

const LogicalPlan& require_right(const LogicalPlan& logical) {
    if (!logical.right) {
        throw std::invalid_argument("logical join node is missing its right input");
    }
    return *logical.right;
}

std::string expression_to_string(const BoundScalarExpr& expression) {
    if (const auto* column = std::get_if<BoundColumnRef>(&expression.value)) {
        if (column->binding.empty()) {
            return "col(" + column->column + ")";
        }
        return "col(" + column->binding + "." + column->column + ")";
    }
    if (const auto* literal = std::get_if<sql::IntLiteral>(&expression.value)) {
        return "lit(" + std::to_string(literal->value) + ")";
    }
    if (const auto* literal = std::get_if<sql::StringLiteral>(&expression.value)) {
        return "lit(" + sql::quote_string_literal(literal->value) + ")";
    }
    return "lit(NULL)";
}

std::string column_to_string(const BoundColumnRef& column) {
    if (column.binding.empty()) {
        return "col(" + column.column + ")";
    }
    return "col(" + column.binding + "." + column.column + ")";
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

std::string comparison_to_string(const BoundComparisonExpr& comparison) {
    return expression_to_string(comparison.left) + " " + comparison_op_to_string(comparison.op) + " " +
           expression_to_string(comparison.right);
}

const BoundPredicate& require_left_predicate(const BoundPredicate& predicate) {
    if (predicate.left == nullptr) {
        throw std::invalid_argument("bound predicate is missing its left child");
    }
    return *predicate.left;
}

const BoundPredicate& require_right_predicate(const BoundPredicate& predicate) {
    if (predicate.right == nullptr) {
        throw std::invalid_argument("bound predicate is missing its right child");
    }
    return *predicate.right;
}

std::string predicate_to_string(const BoundPredicate& predicate) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return comparison_to_string(predicate.comparison);
    case sql::PredicateKind::IsNull:
        return expression_to_string(predicate.null_check) + " IS NULL";
    case sql::PredicateKind::IsNotNull:
        return expression_to_string(predicate.null_check) + " IS NOT NULL";
    case sql::PredicateKind::And:
        return "(" + predicate_to_string(require_left_predicate(predicate)) + " AND " +
               predicate_to_string(require_right_predicate(predicate)) + ")";
    case sql::PredicateKind::Or:
        return "(" + predicate_to_string(require_left_predicate(predicate)) + " OR " +
               predicate_to_string(require_right_predicate(predicate)) + ")";
    }
    throw std::logic_error("unreachable predicate kind");
}

std::string sort_direction_to_string(sql::SortDirection direction) {
    switch (direction) {
    case sql::SortDirection::Asc:
        return "ASC";
    case sql::SortDirection::Desc:
        return "DESC";
    }
    throw std::logic_error("unreachable sort direction");
}

std::string sort_key_to_string(const SortKey& key) {
    return column_to_string(key.column) + " " + sort_direction_to_string(key.direction);
}

std::string aggregate_expression_to_string(const AggregateExpression& aggregate) {
    auto text = aggregate.output_name;
    if (aggregate.argument.has_value()) {
        text += "=" + column_to_string(*aggregate.argument);
    }
    return text;
}

void append_indent(std::ostringstream& out, std::size_t depth) {
    for (std::size_t i = 0; i < depth; ++i) {
        out << "  ";
    }
}

void append_annotation(std::ostringstream& out,
                       const LogicalPlan& logical,
                       const std::function<std::string(const LogicalPlan&)>& annotation) {
    if (!annotation) {
        return;
    }
    const auto text = annotation(logical);
    if (!text.empty()) {
        out << " " << text;
    }
}

void append_plan(std::ostringstream& out,
                 const LogicalPlan& logical,
                 std::size_t depth,
                 const std::function<std::string(const LogicalPlan&)>& annotation = {}) {
    append_indent(out, depth);
    switch (logical.kind) {
    case LogicalKind::Scan:
        out << "Scan[" << logical.table;
        if (logical.binding_name != logical.table) {
            out << " AS " << logical.binding_name;
        }
        out << "]";
        append_annotation(out, logical, annotation);
        return;
    case LogicalKind::Join:
        out << "Join[";
        for (std::size_t i = 0; i < logical.predicates.size(); ++i) {
            if (i != 0) {
                out << " AND ";
            }
            out << predicate_to_string(logical.predicates[i]);
        }
        out << "]";
        append_annotation(out, logical, annotation);
        out << "\n";
        append_plan(out, require_left(logical), depth + 1, annotation);
        out << "\n";
        append_plan(out, require_right(logical), depth + 1, annotation);
        return;
    case LogicalKind::Filter:
        out << "Filter[";
        for (std::size_t i = 0; i < logical.predicates.size(); ++i) {
            if (i != 0) {
                out << " AND ";
            }
            out << predicate_to_string(logical.predicates[i]);
        }
        out << "]";
        append_annotation(out, logical, annotation);
        out << "\n";
        append_plan(out, require_input(logical), depth + 1, annotation);
        return;
    case LogicalKind::Project:
        out << "Project[";
        for (std::size_t i = 0; i < logical.projections.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << logical.projections[i].output_name << "=" << expression_to_string(logical.projections[i].expression);
        }
        out << "]";
        append_annotation(out, logical, annotation);
        out << "\n";
        append_plan(out, require_input(logical), depth + 1, annotation);
        return;
    case LogicalKind::Aggregate:
        out << "Aggregate[group_keys=[";
        for (std::size_t i = 0; i < logical.group_keys.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << column_to_string(logical.group_keys[i]);
        }
        out << "], aggregates=[";
        for (std::size_t i = 0; i < logical.aggregate_expressions.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << aggregate_expression_to_string(logical.aggregate_expressions[i]);
        }
        out << "]]";
        append_annotation(out, logical, annotation);
        out << "\n";
        append_plan(out, require_input(logical), depth + 1, annotation);
        return;
    case LogicalKind::Distinct:
        out << "Distinct";
        append_annotation(out, logical, annotation);
        out << "\n";
        append_plan(out, require_input(logical), depth + 1, annotation);
        return;
    case LogicalKind::Sort:
        out << "Sort[";
        for (std::size_t i = 0; i < logical.sort_keys.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << sort_key_to_string(logical.sort_keys[i]);
        }
        out << "]";
        append_annotation(out, logical, annotation);
        out << "\n";
        append_plan(out, require_input(logical), depth + 1, annotation);
        return;
    case LogicalKind::Limit:
        out << "Limit[" << logical.limit_count << "]";
        append_annotation(out, logical, annotation);
        out << "\n";
        append_plan(out, require_input(logical), depth + 1, annotation);
        return;
    case LogicalKind::Explain:
        out << "Explain";
        append_annotation(out, logical, annotation);
        out << "\n";
        append_plan(out, require_input(logical), depth + 1, annotation);
        return;
    }
    throw std::logic_error("unreachable logical plan kind");
}

} // namespace

std::string to_string(const LogicalPlan& logical) {
    std::ostringstream out;
    append_plan(out, logical, 0);
    return out.str();
}

std::string to_string_annotated(const LogicalPlan& logical,
                                const std::function<std::string(const LogicalPlan&)>& annotation) {
    std::ostringstream out;
    append_plan(out, logical, 0, annotation);
    return out.str();
}

} // namespace plan
